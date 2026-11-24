defmodule Backend.Room do
  @moduledoc """
  One process per room. It owns the whole game: players, phase, timers,
  the word, the strokes and the scores.

  Players are channel processes. Public events go to everyone through the
  endpoint (topic `"room:<code>"`); private ones are sent to a player's pid as
  `{:push, event, payload}` for its channel to push.

  Every timer message carries the turn number it was set for, and a timer
  from an older turn is ignored, so timers never need cancelling.
  """

  use GenServer, restart: :temporary

  alias Backend.{Rules, Words}

  @endpoint BackendWeb.Endpoint

  defstruct code: nil,
            phase: :lobby,
            turn: 0,
            round: 0,
            rounds: 3,
            players: %{},
            order: [],
            host: nil,
            drawer: nil,
            drawn_this_round: MapSet.new(),
            choices: [],
            word: nil,
            revealed: MapSet.new(),
            deadline: nil,
            strokes: [],
            stroke_bytes: 0,
            used_words: MapSet.new(),
            stop_gen: 0

  ## API

  def start_link(code), do: GenServer.start_link(__MODULE__, code, name: via(code))

  def via(code), do: {:via, Registry, {Backend.RoomRegistry, code}}

  def whereis(code) do
    case Registry.lookup(Backend.RoomRegistry, code) do
      [{pid, _}] -> pid
      [] -> nil
    end
  end

  @doc "The room's pid, starting the room if it isn't running."
  def ensure_started(code) do
    case DynamicSupervisor.start_child(Backend.RoomSupervisor, {__MODULE__, code}) do
      {:ok, pid} -> {:ok, pid}
      {:error, {:already_started, pid}} -> {:ok, pid}
      {:error, reason} -> {:error, reason}
    end
  end

  @doc """
  Adds the calling channel process as a player, or takes back the seat of the
  player whose `token` it presents. Replies `{:ok, reply}` with the join
  reply for the client, or `{:error, reason}`.
  """
  def join(room, name, token \\ nil),
    do: GenServer.call(room, {:join, self(), name, token})

  def start(room), do: GenServer.call(room, {:start, self()})
  def choose(room, index), do: GenServer.call(room, {:choose, self(), index})
  def guess(room, text), do: GenServer.call(room, {:guess, self(), text})
  def stroke(room, stroke), do: GenServer.call(room, {:stroke, self(), stroke})
  def undo(room, id), do: GenServer.call(room, {:undo, self(), id})
  def clear(room), do: GenServer.call(room, {:clear, self()})

  @doc "The raw state, for tests and debugging."
  def get_state(room), do: :sys.get_state(room)

  ## Callbacks

  @impl true
  def init(code) do
    state = %__MODULE__{code: code, rounds: Rules.config(:rounds)}
    {:ok, schedule_stop_if_empty(state)}
  end

  @impl true
  def handle_call({:join, pid, name, token}, _from, state) do
    case find_by_token(state, token) do
      nil -> join_new(state, pid, name)
      id -> rejoin(state, id, pid, name)
    end
  end

  def handle_call({:start, pid}, _from, state) do
    with {:ok, id} <- player_id(state, pid),
         :ok <- check(id == state.host, :not_host),
         :ok <- check(state.phase in [:lobby, :game_over], :wrong_phase),
         :ok <- check(connected_count(state) >= Rules.config(:min_players), :not_enough_players) do
      players = Map.new(state.players, fn {id, p} -> {id, %{p | score: 0, guessed: false}} end)

      state =
        %{state | players: players, round: 1, drawn_this_round: MapSet.new(), word: nil}
        |> system_chat("Game started!")
        |> next_turn()

      {:reply, :ok, state}
    else
      {:error, reason} -> {:reply, {:error, reason}, state}
    end
  end

  def handle_call({:choose, pid, index}, _from, state) do
    with {:ok, id} <- player_id(state, pid),
         :ok <- check(id == state.drawer, :not_drawer),
         :ok <- check(state.phase == :choosing, :wrong_phase),
         word when is_binary(word) <- Enum.at(state.choices, index) || {:error, :bad_index} do
      {:reply, :ok, begin_drawing(state, word)}
    else
      {:error, reason} -> {:reply, {:error, reason}, state}
    end
  end

  # Strokes, undo and clear. During a turn only the drawer may draw. In the
  # lobby and after the game ("practice") anyone may, so every stored and
  # broadcast stroke carries its author as `by`: stroke ids are picked by the
  # clients and only unique per author.
  def handle_call({:stroke, pid, stroke}, _from, state) do
    with {:ok, id} <- may_draw(state, pid),
         stroke = Map.put(stroke, :by, id),
         {:ok, state} <- make_room_for(state, stroke_size(stroke)) do
      @endpoint.broadcast_from(pid, topic(state), "stroke", stroke)
      size = stroke_size(stroke)

      {:reply, :ok,
       %{state | strokes: [stroke | state.strokes], stroke_bytes: state.stroke_bytes + size}}
    else
      {:error, reason} -> {:reply, {:error, reason}, state}
    end
  end

  # Undo only ever removes the caller's own stroke.
  def handle_call({:undo, pid, stroke_id}, _from, state) do
    case may_draw(state, pid) do
      {:ok, id} ->
        @endpoint.broadcast_from(pid, topic(state), "undo", %{id: stroke_id, by: id})
        {:reply, :ok, drop_strokes(state, &(&1.by == id and &1.id == stroke_id))}

      error ->
        {:reply, error, state}
    end
  end

  # In a turn the drawer clears the canvas. In practice the host clears the
  # whole canvas (so one player can tidy up after everyone) and anyone else
  # clears only their own strokes; the broadcast's `by` says whose strokes
  # went, and no `by` means all of them.
  def handle_call({:clear, pid}, _from, state) do
    case may_draw(state, pid) do
      {:ok, id} ->
        if practice?(state) and id != state.host do
          @endpoint.broadcast_from(pid, topic(state), "clear", %{by: id})
          {:reply, :ok, drop_strokes(state, &(&1.by == id))}
        else
          @endpoint.broadcast_from(pid, topic(state), "clear", %{})
          {:reply, :ok, drop_strokes(state, fn _ -> true end)}
        end

      error ->
        {:reply, error, state}
    end
  end

  def handle_call({:guess, pid, text}, _from, state) do
    case player_id(state, pid) do
      {:ok, id} -> {:reply, :ok, handle_guess(state, id, text)}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def handle_info({:choose_timeout, turn}, %{turn: turn, phase: :choosing} = state) do
    {:noreply, begin_drawing(state, hd(state.choices))}
  end

  def handle_info({:hint, turn}, %{turn: turn, phase: :drawing} = state) do
    case Rules.reveal_index(state.word, state.revealed) do
      nil -> {:noreply, state}
      i -> {:noreply, broadcast_state(%{state | revealed: MapSet.put(state.revealed, i)})}
    end
  end

  def handle_info({:draw_timeout, turn}, %{turn: turn, phase: :drawing} = state) do
    {:noreply, reveal(state)}
  end

  def handle_info({:reveal_timeout, turn}, %{turn: turn, phase: :reveal} = state) do
    {:noreply, next_turn(state)}
  end

  def handle_info({:DOWN, mref, :process, _pid, _reason}, state) do
    case Enum.find(state.players, fn {_, p} -> p.mref == mref end) do
      nil -> {:noreply, state}
      {id, _} -> {:noreply, disconnect(state, id)}
    end
  end

  def handle_info({:remove, id, mref}, state) do
    case state.players[id] do
      %{connected: false, last_mref: ^mref} -> {:noreply, remove(state, id)}
      _ -> {:noreply, state}
    end
  end

  def handle_info({:stop_if_empty, gen}, %{stop_gen: gen} = state) do
    if connected_count(state) == 0 do
      {:stop, :normal, state}
    else
      {:noreply, state}
    end
  end

  # A timer from an older turn, or a stop check that a join superseded.
  def handle_info(_msg, state), do: {:noreply, state}

  ## Joining and leaving

  defp join_new(state, pid, name) do
    cond do
      map_size(state.players) >= Rules.config(:max_players) ->
        {:reply, {:error, :room_full}, state}

      true ->
        id = new_id(state)
        token = Base.url_encode64(:crypto.strong_rand_bytes(18), padding: false)

        player = %{
          id: id,
          name: name,
          token: token,
          score: 0,
          guessed: false,
          connected: true,
          pid: pid,
          mref: Process.monitor(pid),
          last_mref: nil
        }

        state = %{state | players: Map.put(state.players, id, player), order: state.order ++ [id]}
        state = if state.host, do: state, else: %{state | host: id}
        state = state |> cancel_stop() |> system_chat("#{name} joined")
        {:reply, {:ok, join_reply(state, id)}, after_join(state, id)}
    end
  end

  defp rejoin(state, id, pid, name) do
    player = state.players[id]
    if player.mref, do: Process.demonitor(player.mref, [:flush])
    if player.pid && player.pid != pid, do: send(player.pid, :replaced)

    message =
      if name == player.name,
        do: "#{name} is back",
        else: "#{player.name} is back as #{name}"

    player = %{player | name: name, pid: pid, mref: Process.monitor(pid), connected: true}
    state = %{state | players: Map.put(state.players, id, player)} |> cancel_stop()
    state = if connected?(state, state.host), do: state, else: %{state | host: id}
    state = system_chat(state, message)
    {:reply, {:ok, join_reply(state, id)}, after_join(state, id)}
  end

  # Everyone sees the new player list; the joiner also gets anything private
  # they're owed (choices for a drawer, the word once guessed).
  defp after_join(state, id) do
    state = broadcast_state(state)

    cond do
      id == state.drawer and state.phase == :choosing ->
        push_choices(state)

      id == state.drawer and state.phase == :drawing ->
        push(state, id, "word", %{word: state.word})

      state.phase == :drawing and state.players[id].guessed ->
        push(state, id, "word", %{word: state.word})

      true ->
        :ok
    end

    state
  end

  defp join_reply(state, id) do
    %{
      you: id,
      token: state.players[id].token,
      state: public_state(state),
      strokes: Enum.reverse(state.strokes)
    }
  end

  defp disconnect(state, id) do
    player = state.players[id]
    Process.send_after(self(), {:remove, id, player.mref}, Rules.config(:rejoin_ms))
    player = %{player | connected: false, pid: nil, mref: nil, last_mref: player.mref}

    %{state | players: Map.put(state.players, id, player)}
    |> reassign_host()
    |> system_chat("#{player.name} left")
    |> after_leave(id)
  end

  defp remove(state, id) do
    %{state | players: Map.delete(state.players, id), order: List.delete(state.order, id)}
    |> reassign_host()
    |> broadcast_state()
  end

  defp after_leave(state, id) do
    in_game? = state.phase in [:choosing, :drawing, :reveal]

    cond do
      in_game? and connected_count(state) < Rules.config(:min_players) ->
        state
        |> system_chat("Not enough players, back to the lobby")
        |> to_lobby()

      id == state.drawer and state.phase in [:choosing, :drawing] ->
        reveal(state)

      state.phase == :drawing and everyone_guessed?(state) ->
        reveal(state)

      true ->
        broadcast_state(state)
    end
    |> schedule_stop_if_empty()
  end

  defp reassign_host(state) do
    if connected?(state, state.host) do
      state
    else
      %{state | host: Enum.find(state.order, &connected?(state, &1)) || state.host}
    end
  end

  ## Turn flow

  defp to_lobby(state) do
    %{
      state
      | phase: :lobby,
        turn: state.turn + 1,
        drawer: nil,
        word: nil,
        choices: [],
        deadline: nil,
        strokes: [],
        stroke_bytes: 0
    }
    |> broadcast_state()
  end

  defp next_turn(state) do
    case pick_drawer(state) do
      {:ok, drawer, state} ->
        choices = Words.pick(3, state.used_words)
        turn = state.turn + 1
        Process.send_after(self(), {:choose_timeout, turn}, Rules.config(:choose_ms))

        players = Map.new(state.players, fn {id, p} -> {id, %{p | guessed: false}} end)

        %{
          state
          | phase: :choosing,
            turn: turn,
            drawer: drawer,
            players: players,
            drawn_this_round: MapSet.put(state.drawn_this_round, drawer),
            choices: choices,
            word: nil,
            revealed: MapSet.new(),
            deadline: deadline(Rules.config(:choose_ms)),
            strokes: [],
            stroke_bytes: 0
        }
        |> broadcast_state()
        |> tap(&push_choices/1)

      :game_over ->
        %{state | phase: :game_over, turn: state.turn + 1, drawer: nil, deadline: nil}
        |> recount_ink()
        |> system_chat("Game over!")
        |> broadcast_state()
    end
  end

  defp pick_drawer(state) do
    eligible? = &connected?(state, &1)

    case Rules.next_drawer(state.order, state.drawn_this_round, eligible?) do
      nil when state.round >= state.rounds ->
        :game_over

      nil ->
        state = %{state | round: state.round + 1, drawn_this_round: MapSet.new()}

        case Rules.next_drawer(state.order, state.drawn_this_round, eligible?) do
          nil -> :game_over
          id -> {:ok, id, state}
        end

      id ->
        {:ok, id, state}
    end
  end

  defp begin_drawing(state, word) do
    draw_ms = Rules.config(:draw_ms)
    turn = state.turn
    Process.send_after(self(), {:hint, turn}, div(draw_ms, 2))
    Process.send_after(self(), {:hint, turn}, div(draw_ms * 3, 4))
    Process.send_after(self(), {:draw_timeout, turn}, draw_ms)

    state = %{
      state
      | phase: :drawing,
        word: word,
        choices: [],
        used_words: MapSet.put(state.used_words, word),
        deadline: deadline(draw_ms)
    }

    push(state, state.drawer, "word", %{word: word})
    broadcast_state(state)
  end

  defp reveal(state) do
    turn = state.turn + 1
    reveal_ms = Rules.config(:reveal_ms)
    Process.send_after(self(), {:reveal_timeout, turn}, reveal_ms)

    state = %{state | phase: :reveal, turn: turn, deadline: deadline(reveal_ms)}

    if state.word,
      do: system_chat(state, "The word was #{state.word}") |> broadcast_state(),
      else: broadcast_state(state)
  end

  ## Guessing and chat

  defp handle_guess(state, id, text) do
    player = state.players[id]
    knows_word? = state.phase == :drawing and (id == state.drawer or player.guessed)

    cond do
      knows_word? ->
        if String.contains?(Rules.normalize(text), Rules.normalize(state.word)) do
          push(state, id, "chat", chat(nil, "You can't say the word", "system"))
        else
          payload = chat(player, text, "secret")

          for {pid_id, p} <- state.players,
              p.connected,
              pid_id == state.drawer or p.guessed,
              do: push(state, pid_id, "chat", payload)
        end

        state

      state.phase == :drawing and Rules.correct_guess?(text, state.word) ->
        correct_guess(state, id)

      true ->
        if state.phase == :drawing and Rules.close_guess?(text, state.word),
          do: push(state, id, "close", %{text: text})

        @endpoint.broadcast(topic(state), "chat", chat(player, text, "msg"))
        state
    end
  end

  defp correct_guess(state, id) do
    points = Rules.guesser_points(time_left(state), Rules.config(:draw_ms))

    players =
      state.players
      |> Map.update!(id, &%{&1 | guessed: true, score: &1.score + points})
      |> Map.update!(state.drawer, &%{&1 | score: &1.score + Rules.drawer_points_per_guess()})

    state = %{state | players: players}
    push(state, id, "word", %{word: state.word})

    @endpoint.broadcast(
      topic(state),
      "chat",
      chat(players[id], "#{players[id].name} guessed the word!", "correct")
    )

    if everyone_guessed?(state), do: reveal(state), else: broadcast_state(state)
  end

  defp everyone_guessed?(state) do
    guessers = for {id, p} <- state.players, p.connected, id != state.drawer, do: p
    guessers != [] and Enum.all?(guessers, & &1.guessed)
  end

  defp system_chat(state, text) do
    @endpoint.broadcast(topic(state), "chat", chat(nil, text, "system"))
    state
  end

  defp chat(nil, text, kind), do: %{from: nil, from_id: nil, text: text, kind: kind}

  defp chat(player, text, kind),
    do: %{from: player.name, from_id: player.id, text: text, kind: kind}

  ## Output

  defp broadcast_state(state) do
    @endpoint.broadcast(topic(state), "state", public_state(state))
    state
  end

  defp push_choices(state) do
    push(state, state.drawer, "choices", %{words: state.choices, time_left_ms: time_left(state)})
  end

  defp push(state, id, event, payload) do
    case state.players[id] do
      %{pid: pid} when is_pid(pid) -> send(pid, {:push, event, payload})
      _ -> :ok
    end
  end

  @doc false
  def public_state(state) do
    players =
      for id <- state.order, p = state.players[id] do
        %{
          id: id,
          name: p.name,
          score: p.score,
          guessed: p.guessed,
          drawing: id == state.drawer,
          connected: p.connected
        }
      end

    %{
      phase: Atom.to_string(state.phase),
      turn: state.turn,
      round: state.round,
      rounds: state.rounds,
      players: players,
      host: state.host,
      drawer: state.drawer,
      hint: if(state.phase == :drawing, do: Rules.hint(state.word, state.revealed), else: ""),
      time_left_ms: time_left(state),
      word: if(state.phase in [:reveal, :game_over], do: state.word)
    }
  end

  ## Helpers

  defp topic(state), do: "room:" <> state.code

  defp deadline(ms), do: System.monotonic_time(:millisecond) + ms

  defp time_left(%{deadline: nil}), do: 0
  defp time_left(%{deadline: d}), do: max(d - System.monotonic_time(:millisecond), 0)

  defp player_id(state, pid) do
    case Enum.find(state.players, fn {_, p} -> p.pid == pid end) do
      {id, _} -> {:ok, id}
      nil -> {:error, :not_in_room}
    end
  end

  # The lobby and the game-over screen are a shared practice canvas.
  defp practice?(state), do: state.phase in [:lobby, :game_over]

  defp may_draw(state, pid) do
    with {:ok, id} <- player_id(state, pid) do
      if practice?(state) do
        {:ok, id}
      else
        with :ok <- check(id == state.drawer, :not_drawer),
             :ok <- check(state.phase == :drawing, :wrong_phase),
             do: {:ok, id}
      end
    end
  end

  defp check(true, _reason), do: :ok
  defp check(false, reason), do: {:error, reason}

  defp find_by_token(_state, nil), do: nil

  defp find_by_token(state, token) do
    Enum.find_value(state.players, fn {id, p} -> if p.token == token, do: id end)
  end

  defp connected?(state, id), do: match?(%{connected: true}, state.players[id])

  defp connected_count(state), do: Enum.count(state.players, fn {_, p} -> p.connected end)

  defp new_id(state) do
    id = Base.url_encode64(:crypto.strong_rand_bytes(6), padding: false)
    if Map.has_key?(state.players, id), do: new_id(state), else: id
  end

  defp stroke_size(stroke), do: 16 + 4 * length(stroke.pts)

  ## Ink

  # Removes the stored stroke chunks matching `fun`. In a turn the ink spent
  # stays spent, so a drawer can't undo and redraw forever; in practice the
  # count follows what is on the canvas, since the canvas lives on.
  defp drop_strokes(state, fun) do
    {gone, kept} = Enum.split_with(state.strokes, fun)

    if practice?(state) do
      freed = gone |> Enum.map(&stroke_size/1) |> Enum.sum()
      %{state | strokes: kept, stroke_bytes: state.stroke_bytes - freed}
    else
      %{state | strokes: kept}
    end
  end

  # During a turn a full canvas refuses more ink. In practice the oldest
  # strokes are wiped instead (everyone is told with an "undo"), so a long
  # doodling session never locks up.
  defp make_room_for(state, size) do
    max = Rules.config(:max_stroke_bytes)

    cond do
      state.stroke_bytes + size <= max -> {:ok, state}
      not practice?(state) or state.strokes == [] -> {:error, :too_much_ink}
      true -> state |> drop_oldest_stroke() |> make_room_for(size)
    end
  end

  defp drop_oldest_stroke(state) do
    %{id: id, by: by} = List.last(state.strokes)
    @endpoint.broadcast(topic(state), "undo", %{id: id, by: by})
    drop_strokes(state, &(&1.by == by and &1.id == id))
  end

  # The ink count of what is on the canvas now, for when a game ends and its
  # last drawing becomes the practice canvas.
  defp recount_ink(state) do
    %{state | stroke_bytes: state.strokes |> Enum.map(&stroke_size/1) |> Enum.sum()}
  end

  defp cancel_stop(state), do: %{state | stop_gen: state.stop_gen + 1}

  defp schedule_stop_if_empty(state) do
    if connected_count(state) == 0 do
      state = cancel_stop(state)
      Process.send_after(self(), {:stop_if_empty, state.stop_gen}, Rules.config(:empty_room_ms))
      state
    else
      state
    end
  end
end

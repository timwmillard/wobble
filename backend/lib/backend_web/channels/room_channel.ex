defmodule BackendWeb.RoomChannel do
  @moduledoc """
  Thin channel in front of a `Backend.Room`: it checks the shape of every
  message, applies per-player rate limits, and forwards to the room. The room
  decides what is allowed for the player's role and the phase.

  The room sends private events as `{:push, event, payload}`; public events
  arrive as endpoint broadcasts and are pushed by Phoenix directly.
  """

  use BackendWeb, :channel

  alias Backend.{Room, Rules}

  @canvas_w 800
  @canvas_h 600
  @max_id 2_147_483_647

  # Drawer: at most this many stroke/undo/clear messages per second.
  @stroke_rate 60
  # Chat: at most @chat_burst messages per @chat_window_ms.
  @chat_burst 5
  @chat_window_ms 3_000

  @impl true
  def join("room:" <> code, payload, socket) do
    with true <- Rules.valid_room_code?(code) || {:error, :invalid_room},
         {:ok, name} <- Rules.clean_name(payload["name"]),
         token = clean_token(payload["token"]),
         {:ok, reply} <- join_room(code, name, token) do
      room = Room.whereis(code)
      Process.monitor(room)

      {:ok, reply,
       assign(socket,
         room: room,
         player_id: reply.you,
         client: payload["client"],
         stroke_window: {0, 0},
         chat_times: []
       )}
    else
      {:error, reason} -> {:error, %{reason: Atom.to_string(reason)}}
    end
  end

  # The room may be shutting down just as we join; start a fresh one once.
  defp join_room(code, name, token, attempts \\ 2) do
    {:ok, room} = Room.ensure_started(code)
    Room.join(room, name, token)
  catch
    :exit, _ when attempts > 1 -> join_room(code, name, token, attempts - 1)
    :exit, _ -> {:error, :room_unavailable}
  end

  @impl true
  def handle_in("start", _payload, socket), do: respond(Room.start(socket.assigns.room), socket)

  def handle_in("choose", %{"index" => i}, socket) when i in 0..2,
    do: respond(Room.choose(socket.assigns.room, i), socket)

  def handle_in("guess", %{"text" => text}, socket) when is_binary(text) do
    text = text |> String.trim() |> String.replace(~r/\s+/u, " ")
    max = Rules.config(:max_guess_length)

    cond do
      text == "" or not String.printable?(text) -> respond({:error, :invalid}, socket)
      String.length(text) > max -> respond({:error, :too_long}, socket)
      true -> with_chat_limit(socket, &respond(Room.guess(&1.assigns.room, text), &1))
    end
  end

  def handle_in("stroke", payload, socket) do
    case parse_stroke(payload) do
      {:ok, stroke} ->
        with_stroke_limit(socket, &noreply(Room.stroke(&1.assigns.room, stroke), &1))

      :error ->
        respond({:error, :invalid}, socket)
    end
  end

  def handle_in("undo", %{"id" => id}, socket) when is_integer(id) and id in 0..@max_id,
    do: with_stroke_limit(socket, &respond(Room.undo(&1.assigns.room, id), &1))

  def handle_in("clear", _payload, socket),
    do: with_stroke_limit(socket, &respond(Room.clear(&1.assigns.room), &1))

  def handle_in(_event, _payload, socket), do: respond({:error, :invalid}, socket)

  @impl true
  def handle_info({:push, event, payload}, socket) do
    push(socket, event, payload)
    {:noreply, socket}
  end

  # Another connection took over this player's seat.
  def handle_info(:replaced, socket), do: {:stop, :normal, socket}

  def handle_info({:DOWN, _, :process, room, _}, %{assigns: %{room: room}} = socket),
    do: {:stop, :normal, socket}

  def handle_info(_msg, socket), do: {:noreply, socket}

  ## Payload checks

  @doc false
  def parse_stroke(%{"id" => id, "pts" => pts, "c" => c, "s" => s} = payload)
      when is_integer(id) and id in 0..@max_id and is_list(pts) and is_integer(c) and c in 0..15 and
             is_integer(s) and s in 1..40 do
    max = Rules.config(:max_points_per_chunk) * 2
    n = length(pts)

    with true <- n >= 2 and n <= max and rem(n, 2) == 0,
         true <- Enum.all?(pts, &is_integer/1),
         {:ok, pressure} <- parse_pressure(payload["p"], div(n, 2)) do
      stroke = %{id: id, pts: clamp_points(pts), c: c, s: s, end: payload["end"] == true}
      {:ok, if(pressure, do: Map.put(stroke, :p, pressure), else: stroke)}
    else
      _ -> :error
    end
  end

  def parse_stroke(_), do: :error

  defp parse_pressure(nil, _count), do: {:ok, nil}

  defp parse_pressure(p, count) when is_list(p) and length(p) == count do
    if Enum.all?(p, &(is_integer(&1) and &1 in 0..255)), do: {:ok, p}, else: :error
  end

  defp parse_pressure(_, _), do: :error

  defp clamp_points(pts) do
    pts
    |> Enum.chunk_every(2)
    |> Enum.flat_map(fn [x, y] -> [min(max(x, 0), @canvas_w), min(max(y, 0), @canvas_h)] end)
  end

  defp clean_token(token) when is_binary(token) and byte_size(token) in 1..64, do: token
  defp clean_token(_), do: nil

  ## Rate limits

  defp with_stroke_limit(socket, fun) do
    now_s = System.monotonic_time(:second)

    {window, count} =
      case socket.assigns.stroke_window do
        {^now_s, count} -> {now_s, count + 1}
        _ -> {now_s, 1}
      end

    socket = assign(socket, :stroke_window, {window, count})
    if count > @stroke_rate, do: respond({:error, :rate_limited}, socket), else: fun.(socket)
  end

  defp with_chat_limit(socket, fun) do
    now = System.monotonic_time(:millisecond)
    recent = Enum.filter(socket.assigns.chat_times, &(now - &1 < @chat_window_ms))

    if length(recent) >= @chat_burst do
      respond({:error, :rate_limited}, assign(socket, :chat_times, recent))
    else
      fun.(assign(socket, :chat_times, [now | recent]))
    end
  end

  ## Replies

  defp respond(:ok, socket), do: {:reply, :ok, socket}

  defp respond({:error, reason}, socket),
    do: {:reply, {:error, %{reason: Atom.to_string(reason)}}, socket}

  # Strokes are fire-and-forget; only failures are reported.
  defp noreply(:ok, socket), do: {:noreply, socket}
  defp noreply(error, socket), do: respond(error, socket)
end

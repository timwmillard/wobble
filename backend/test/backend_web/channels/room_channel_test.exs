defmodule BackendWeb.RoomChannelTest do
  use BackendWeb.ChannelCase, async: true

  setup do
    {:ok, code: "t#{System.unique_integer([:positive])}"}
  end

  # The next message of `event` for player `name`, skipping others.
  defp next(name, event, timeout \\ 1000) do
    receive do
      {:msg, ^name, ^event, payload} -> payload
    after
      timeout -> flunk("#{name} never got #{event}")
    end
  end

  defp next_state(name, phase) do
    receive do
      {:msg, ^name, "state", %{phase: ^phase} = s} -> s
    after
      2000 -> flunk("#{name} never saw phase #{phase}")
    end
  end

  defp by_id(players, id), do: Enum.find(players, &(&1.id == id))

  defp start_game(code, names) do
    players = Enum.map(names, &TestPlayer.join(code, &1))
    [host | _] = players
    assert {:ok, _} = TestPlayer.send_event(host, "start")
    state = next_state(host.name, "choosing")
    {players, state}
  end

  describe "join" do
    test "replies with the player's id, a token and the room state", %{code: code} do
      ana = TestPlayer.join(code, "  Ana ")
      assert %{you: id, token: token, state: state, strokes: []} = ana.join
      assert is_binary(token)
      assert state.phase == "lobby"
      assert state.host == id
      assert [%{id: ^id, name: "Ana", score: 0, connected: true}] = state.players
    end

    test "rejects bad names and room codes", %{code: code} do
      assert {:error, %{reason: "invalid_name"}} = TestPlayer.join(code, "")
      assert {:error, %{reason: "invalid_room"}} = TestPlayer.join("Bad Code!", "Ana")
    end

    test "rejects a full room", %{code: code} do
      for i <- 1..12, do: TestPlayer.join(code, "p#{i}")
      assert {:error, %{reason: "room_full"}} = TestPlayer.join(code, "late")
    end
  end

  describe "start" do
    test "needs the host and two players", %{code: code} do
      ana = TestPlayer.join(code, "Ana")
      assert {:error, %{reason: "not_enough_players"}} = TestPlayer.send_event(ana, "start")

      bo = TestPlayer.join(code, "Bo")
      assert {:error, %{reason: "not_host"}} = TestPlayer.send_event(bo, "start")
      assert {:ok, _} = TestPlayer.send_event(ana, "start")
    end
  end

  describe "a turn" do
    test "only the drawer gets the choices and the word", %{code: code} do
      {players, state} = start_game(code, ["Ana", "Bo", "Cy"])
      drawer = by_id(players, state.drawer)
      [g1, g2] = players -- [drawer]

      assert %{words: [_, _, _] = words} = next(drawer.name, "choices")
      refute_received {:msg, _, "choices", _}

      assert {:ok, _} = TestPlayer.send_event(drawer, "choose", %{"index" => 1})
      word = Enum.at(words, 1)
      assert %{word: ^word} = next(drawer.name, "word")

      s = next_state(g1.name, "drawing")
      assert s.word == nil
      assert String.length(s.hint) == String.length(word)
      refute s.hint =~ ~r/[a-z]/

      # A wrong guess is public chat; the right one gives the word only to
      # that guesser.
      assert {:ok, _} = TestPlayer.send_event(g1, "guess", %{"text" => "zzzz"})
      g2n = g2.name
      assert_receive {:msg, ^g2n, "chat", %{text: "zzzz", kind: "msg"}}

      assert {:ok, _} = TestPlayer.send_event(g1, "guess", %{"text" => String.upcase(word)})
      assert %{word: ^word} = next(g1.name, "word")
      assert_receive {:msg, ^g2n, "chat", %{kind: "correct"}}
      refute_received {:msg, _, "word", _}

      # Nothing g2 saw before guessing carries the word.
      assert_no_word(g2.name, word)

      assert {:ok, _} = TestPlayer.send_event(g2, "guess", %{"text" => word})
      s = next_state(g2.name, "reveal")
      assert s.word == word
      scores = Map.new(s.players, &{&1.id, &1.score})
      assert scores[g1.id] >= scores[g2.id] and scores[g2.id] > 50
      assert scores[drawer.id] == 50

      # The reveal ends by itself and the next drawer is up.
      g1n = g1.name
      old = drawer.id

      assert_receive {:msg, ^g1n, "state", %{phase: "choosing", drawer: new}} when new != old,
                     1000
    end

    test "strokes, undo and clear go to everyone but the drawer", %{code: code} do
      {players, state} = start_game(code, ["Ana", "Bo"])
      drawer = by_id(players, state.drawer)
      [guesser] = players -- [drawer]

      assert {:error, %{reason: "wrong_phase"}} =
               TestPlayer.send_event(drawer, "clear")

      TestPlayer.send_event(drawer, "choose", %{"index" => 0})
      next_state(guesser.name, "drawing")

      stroke = %{"id" => 7, "pts" => [10, 20, 900, -5], "c" => 3, "s" => 5, "end" => true}
      assert nil == TestPlayer.send_event(drawer, "stroke", stroke)
      assert %{id: 7, pts: [10, 20, 800, 0], c: 3, s: 5, end: true} = next(guesser.name, "stroke")

      assert {:error, %{reason: "not_drawer"}} = TestPlayer.send_event(guesser, "stroke", stroke)

      assert {:error, %{reason: "invalid"}} =
               TestPlayer.send_event(drawer, "stroke", %{stroke | "c" => 99})

      # A late joiner gets the strokes so far.
      late = TestPlayer.join(code, "Late")
      assert [%{id: 7}] = late.join.strokes

      assert {:ok, _} = TestPlayer.send_event(drawer, "undo", %{"id" => 7})
      assert %{id: 7} = next(guesser.name, "undo")
      assert {:ok, _} = TestPlayer.send_event(drawer, "clear")
      assert %{} = next(guesser.name, "clear")
      dn = drawer.name
      refute_received {:msg, ^dn, "stroke", _}
    end

    test "close guesses and secret chat", %{code: code} do
      {players, state} = start_game(code, ["Ana", "Bo", "Cy"])
      drawer = by_id(players, state.drawer)
      [g1, g2] = players -- [drawer]

      %{words: words} = next(drawer.name, "choices")
      long = Enum.find_index(words, &(String.length(&1) >= 4 and not String.contains?(&1, " ")))

      if long do
        TestPlayer.send_event(drawer, "choose", %{"index" => long})
        word = Enum.at(words, long)
        next_state(g1.name, "drawing")

        TestPlayer.send_event(g1, "guess", %{"text" => String.slice(word, 0..-2//1)})
        assert %{} = next(g1.name, "close")
        refute_received {:msg, _, "close", _}

        TestPlayer.send_event(g1, "guess", %{"text" => word})
        next(g1.name, "word")
        drain()

        # After guessing, g1's chat reaches only the drawer.
        TestPlayer.send_event(g1, "guess", %{"text" => "nice drawing"})
        assert %{kind: "secret", text: "nice drawing"} = next(drawer.name, "chat")
        g2n = g2.name
        refute_receive {:msg, ^g2n, "chat", %{text: "nice drawing"}}, 100

        # ...and can't give the word away.
        TestPlayer.send_event(g1, "guess", %{"text" => "it is #{word}"})
        g1n = g1.name
        assert_receive {:msg, ^g1n, "chat", %{kind: "system", text: "You can't" <> _}}
        refute_receive {:msg, _, "chat", %{text: "it is " <> _}}, 100
      end
    end

    test "the server picks a word when the drawer doesn't", %{code: code} do
      {players, state} = start_game(code, ["Ana", "Bo"])
      drawer = by_id(players, state.drawer)
      %{words: [first | _]} = next(drawer.name, "choices")
      assert %{word: ^first} = next(drawer.name, "word", 1000)
    end
  end

  describe "leaving" do
    test "the drawer leaving ends the turn", %{code: code} do
      {players, state} = start_game(code, ["Ana", "Bo", "Cy"])
      drawer = by_id(players, state.drawer)
      [g1 | _] = players -- [drawer]

      TestPlayer.disconnect(drawer)
      s = next_state(g1.name, "reveal")
      assert %{connected: false} = Enum.find(s.players, &(&1.id == drawer.id))
    end

    test "dropping below two players returns to the lobby", %{code: code} do
      {[ana, bo], _} = start_game(code, ["Ana", "Bo"])
      TestPlayer.disconnect(bo)
      s = next_state(ana.name, "lobby")
      assert s.host == ana.id
    end

    test "a player who comes back with their token keeps their seat", %{code: code} do
      ana = TestPlayer.join(code, "Ana")
      bo = TestPlayer.join(code, "Bo")
      TestPlayer.disconnect(ana)
      s = next(bo.name, "state")
      assert %{connected: false} = Enum.find(s.players, &(&1.id == ana.id))
      assert s.host == bo.id

      again = TestPlayer.join(code, "Ana", %{"token" => ana.token})
      assert again.id == ana.id
      assert length(again.join.state.players) == 2
    end

    test "a player who comes back under a new name is renamed", %{code: code} do
      ana = TestPlayer.join(code, "Ana")
      _bo = TestPlayer.join(code, "Bo")
      TestPlayer.disconnect(ana)

      again = TestPlayer.join(code, "Anna", %{"token" => ana.token})
      assert again.id == ana.id
      assert %{name: "Anna"} = Enum.find(again.join.state.players, &(&1.id == ana.id))
    end

    test "a player gone too long is removed", %{code: code} do
      ana = TestPlayer.join(code, "Ana")
      bo = TestPlayer.join(code, "Bo")
      TestPlayer.disconnect(ana)

      bo_id = bo.id
      assert_receive {:msg, "Bo", "state", %{players: [%{id: ^bo_id}]}}, 1000
    end
  end

  test "two players finish a whole game", %{code: code} do
    {players, _} = start_game(code, ["Ana", "Bo"])
    ids = Map.new(players, &{&1.id, &1})
    final = play_until_over(ids, "Ana", 0)

    assert final.phase == "game_over"
    assert final.round == 3
    assert Enum.all?(final.players, &(&1.score > 0))
  end

  # Drives every turn quickly: the drawer chooses, the other player guesses.
  defp play_until_over(ids, watcher, turns) do
    assert turns < 40

    receive do
      {:msg, _, "choices", _} = msg ->
        {:msg, name, _, _} = msg
        player = Enum.find(Map.values(ids), &(&1.name == name))
        TestPlayer.send_event(player, "choose", %{"index" => 0})
        play_until_over(ids, watcher, turns)

      {:msg, name, "word", %{word: word}} ->
        drawer = Enum.find(Map.values(ids), &(&1.name == name))
        [guesser] = Map.values(ids) -- [drawer]
        TestPlayer.send_event(guesser, "guess", %{"text" => word})
        play_until_over(ids, watcher, turns + 1)

      {:msg, ^watcher, "state", %{phase: "game_over"} = s} ->
        s

      _ ->
        play_until_over(ids, watcher, turns)
    after
      3000 -> flunk("game stalled")
    end
  end

  defp assert_no_word(name, word) do
    receive do
      {:msg, ^name, event, payload} ->
        refute event == "word"
        refute inspect(payload) =~ word
        assert_no_word(name, word)
    after
      0 -> :ok
    end
  end

  defp drain do
    receive do
      {:msg, _, _, _} -> drain()
    after
      20 -> :ok
    end
  end
end

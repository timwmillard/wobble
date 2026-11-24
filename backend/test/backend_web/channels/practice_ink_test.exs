defmodule BackendWeb.PracticeInkTest do
  # Not async: it shrinks the global ink cap.
  use BackendWeb.ChannelCase, async: false

  setup do
    old = Application.get_env(:backend, :rules, [])
    # Room for three 2-point chunks (16 + 4 * 4 = 32 bytes each).
    Application.put_env(:backend, :rules, Keyword.put(old, :max_stroke_bytes, 96))
    on_exit(fn -> Application.put_env(:backend, :rules, old) end)
    {:ok, code: "ink#{System.unique_integer([:positive])}"}
  end

  defp stroke(id), do: %{"id" => id, "pts" => [1, 2, 3, 4], "c" => 1, "s" => 4, "end" => true}

  test "a full practice canvas drops its oldest strokes", %{code: code} do
    ana = TestPlayer.join(code, "Ana")
    bo = TestPlayer.join(code, "Bo")

    for id <- 1..3, do: TestPlayer.send_event(ana, "stroke", stroke(id))
    assert nil == TestPlayer.send_event(bo, "stroke", stroke(1))

    ana_id = ana.id
    assert_receive {:msg, "Bo", "undo", %{id: 1, by: ^ana_id}}
    # The author is told too: the server dropped it, not them.
    assert_receive {:msg, "Ana", "undo", %{id: 1, by: ^ana_id}}

    bo_id = bo.id

    assert [%{id: 2}, %{id: 3}, %{id: 1, by: ^bo_id}] =
             TestPlayer.join(code, "Cy").join.strokes

    # Undo frees ink too, so nothing more is dropped.
    assert {:ok, _} = TestPlayer.send_event(ana, "undo", %{"id" => 2})
    assert nil == TestPlayer.send_event(bo, "stroke", stroke(2))
    refute_receive {:msg, "Ana", "undo", _}, 50
  end
end

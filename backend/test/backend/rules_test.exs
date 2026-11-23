defmodule Backend.RulesTest do
  use ExUnit.Case, async: true

  alias Backend.Rules

  test "normalize ignores case and extra whitespace" do
    assert Rules.normalize("  Ice   CREAM ") == "ice cream"
    assert Rules.correct_guess?("ice  cream", "Ice Cream")
    refute Rules.correct_guess?("icecream", "ice cream")
  end

  test "close guesses are one edit away from words of 4+ letters" do
    assert Rules.close_guess?("hors", "horse")
    assert Rules.close_guess?("horsse", "horse")
    assert Rules.close_guess?("hoxse", "horse")
    refute Rules.close_guess?("horse", "horse")
    refute Rules.close_guess?("hrse!", "horse")
    refute Rules.close_guess?("ca", "cat")
  end

  test "guesser points fall from 300 to 50 over the draw time" do
    assert Rules.guesser_points(80_000, 80_000) == 300
    assert Rules.guesser_points(40_000, 80_000) == 175
    assert Rules.guesser_points(0, 80_000) == 50
    assert Rules.guesser_points(-5, 80_000) == 50
  end

  test "hint hides letters but not spaces" do
    assert Rules.hint("ice cream", MapSet.new()) == "___ _____"
    assert Rules.hint("ice cream", MapSet.new([0, 4])) == "i__ c____"
    assert Rules.hint("yo-yo", MapSet.new()) == "__-__"
  end

  test "reveal_index never reveals the last hidden letter" do
    assert Rules.reveal_index("ab", MapSet.new([0])) == nil
    assert Rules.reveal_index("ab", MapSet.new(), fn _ -> 2 end) == 1
    assert Rules.reveal_index("a b", MapSet.new(), fn _ -> 2 end) == 2
  end

  test "next_drawer takes the first eligible player who hasn't drawn" do
    order = ["a", "b", "c"]
    all = fn _ -> true end
    assert Rules.next_drawer(order, MapSet.new(), all) == "a"
    assert Rules.next_drawer(order, MapSet.new(["a"]), all) == "b"
    assert Rules.next_drawer(order, MapSet.new(["a"]), &(&1 != "b")) == "c"
    assert Rules.next_drawer(order, MapSet.new(order), all) == nil
  end

  test "names are trimmed and limited to 16 printable characters" do
    assert Rules.clean_name("  Ana  ") == {:ok, "Ana"}
    assert Rules.clean_name("") == {:error, :invalid_name}
    assert Rules.clean_name(String.duplicate("x", 17)) == {:error, :invalid_name}
    assert Rules.clean_name("a\u0007b") == {:error, :invalid_name}
    assert Rules.clean_name(nil) == {:error, :invalid_name}
  end

  test "room codes" do
    assert Rules.valid_room_code?("abcd-12")
    refute Rules.valid_room_code?("ABCD")
    refute Rules.valid_room_code?(String.duplicate("a", 25))
  end
end

defmodule Backend.Rules do
  @moduledoc """
  Pure game rules: scoring, hints, guess matching and drawer rotation.

  Nothing here touches processes or time, so it is tested directly.
  """

  @defaults [
    rounds: 3,
    min_players: 2,
    max_players: 12,
    choose_ms: 15_000,
    draw_ms: 80_000,
    reveal_ms: 5_000,
    rejoin_ms: 30_000,
    empty_room_ms: 300_000,
    max_stroke_bytes: 2_000_000,
    max_points_per_chunk: 256,
    max_guess_length: 100
  ]

  @doc "The rule settings, from `config :backend, :rules` over the defaults."
  def config do
    Keyword.merge(@defaults, Application.get_env(:backend, :rules, []))
  end

  def config(key), do: Keyword.fetch!(config(), key)

  @doc """
  Lowercases, trims and collapses inner whitespace, so "  Ice   Cream " and
  "ice cream" match.
  """
  def normalize(text) when is_binary(text) do
    text
    |> String.downcase()
    |> String.split()
    |> Enum.join(" ")
  end

  def correct_guess?(guess, word), do: normalize(guess) == normalize(word)

  @doc """
  A guess one edit away from a word of 4 or more letters. Exact matches are
  not close.
  """
  def close_guess?(guess, word) do
    g = normalize(guess)
    w = normalize(word)
    String.length(w) >= 4 and g != w and levenshtein(g, w) == 1
  end

  @doc "Points for a correct guess with `time_left_ms` of `draw_ms` remaining."
  def guesser_points(time_left_ms, draw_ms) when draw_ms > 0 do
    fraction = min(max(time_left_ms, 0), draw_ms) / draw_ms
    round(50 + 250 * fraction)
  end

  def drawer_points_per_guess, do: 25

  @doc """
  The word as shown to guessers: letters at `revealed` indices are shown, other
  letters are underscores, and anything that isn't a letter (spaces, hyphens)
  is shown as is.
  """
  def hint(nil, _revealed), do: ""

  def hint(word, revealed) do
    word
    |> String.graphemes()
    |> Enum.with_index()
    |> Enum.map_join(fn {ch, i} ->
      if letter?(ch) and not MapSet.member?(revealed, i), do: "_", else: ch
    end)
  end

  @doc """
  The index of a letter to reveal next, picked with `rand_fun` (given the
  number of candidates, returns a 0-based pick). Returns nil when revealing
  one would leave no hidden letter.
  """
  def reveal_index(word, revealed, rand_fun \\ &:rand.uniform(&1)) do
    hidden =
      word
      |> String.graphemes()
      |> Enum.with_index()
      |> Enum.filter(fn {ch, i} -> letter?(ch) and not MapSet.member?(revealed, i) end)
      |> Enum.map(&elem(&1, 1))

    case hidden do
      [] -> nil
      [_] -> nil
      _ -> Enum.at(hidden, rand_fun.(length(hidden)) - 1)
    end
  end

  @doc """
  The next drawer: the first id in `order` that is `eligible?` and hasn't
  drawn yet this round, or nil when everyone has.
  """
  def next_drawer(order, drawn, eligible?) do
    Enum.find(order, fn id -> eligible?.(id) and not MapSet.member?(drawn, id) end)
  end

  @doc "Trims and validates a player name: 1–16 printable characters."
  def clean_name(name) when is_binary(name) do
    name = name |> String.trim() |> String.replace(~r/\s+/u, " ")

    cond do
      not String.printable?(name) -> {:error, :invalid_name}
      String.match?(name, ~r/[\x00-\x1f\x7f]/) -> {:error, :invalid_name}
      String.length(name) < 1 -> {:error, :invalid_name}
      String.length(name) > 16 -> {:error, :invalid_name}
      true -> {:ok, name}
    end
  end

  def clean_name(_), do: {:error, :invalid_name}

  def valid_room_code?(code) when is_binary(code), do: Regex.match?(~r/^[a-z0-9-]{1,24}$/, code)
  def valid_room_code?(_), do: false

  defp letter?(ch), do: String.match?(ch, ~r/^[\p{L}\p{N}]$/u)

  defp levenshtein(a, b) do
    a = String.graphemes(a)
    b = String.graphemes(b)
    first = Enum.to_list(0..length(b))

    a
    |> Enum.with_index(1)
    |> Enum.reduce(first, fn {ca, i}, prev ->
      {row, _} =
        b
        |> Enum.zip(prev)
        |> Enum.zip(tl(prev))
        |> Enum.reduce({[i], i}, fn {{cb, diag}, up}, {acc, left} ->
          cost = if ca == cb, do: 0, else: 1
          v = Enum.min([left + 1, up + 1, diag + cost])
          {[v | acc], v}
        end)

      Enum.reverse(row)
    end)
    |> List.last()
  end
end

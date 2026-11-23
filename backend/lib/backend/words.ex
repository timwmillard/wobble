defmodule Backend.Words do
  @moduledoc "The word list, read from `priv/words/en.txt` at compile time."

  @path Path.join([__DIR__, "..", "..", "priv", "words", "en.txt"])
  @external_resource @path

  @words @path
         |> File.read!()
         |> String.split("\n")
         |> Enum.map(&String.trim/1)
         |> Enum.reject(&(&1 == ""))
         |> Enum.uniq()

  def all, do: @words

  @doc """
  `n` distinct random words, avoiding `exclude` (a MapSet) while enough
  other words remain.
  """
  def pick(n, exclude \\ MapSet.new()) do
    fresh = Enum.reject(@words, &MapSet.member?(exclude, &1))
    pool = if length(fresh) >= n, do: fresh, else: @words
    Enum.take_random(pool, n)
  end
end

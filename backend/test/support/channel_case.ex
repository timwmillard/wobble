defmodule BackendWeb.ChannelCase do
  @moduledoc """
  The test case for channel tests. `BackendWeb.TestPlayer` runs each player
  of a room in its own process, so several players can share a test.
  """

  use ExUnit.CaseTemplate

  using do
    quote do
      import Phoenix.ChannelTest
      import BackendWeb.ChannelCase
      alias BackendWeb.TestPlayer

      @endpoint BackendWeb.Endpoint
    end
  end
end

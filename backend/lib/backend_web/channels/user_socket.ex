defmodule BackendWeb.UserSocket do
  use Phoenix.Socket

  channel "room:*", BackendWeb.RoomChannel

  # Anyone may connect; a player's identity comes from the room on join.
  @impl true
  def connect(_params, socket, _connect_info), do: {:ok, socket}

  @impl true
  def id(_socket), do: nil
end

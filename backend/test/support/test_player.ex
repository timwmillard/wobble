defmodule BackendWeb.TestPlayer do
  @moduledoc """
  A fake client: joins a room in its own process and forwards everything it
  receives to the test process as `{:msg, name, event, payload}`.
  """

  import Phoenix.ChannelTest

  @endpoint BackendWeb.Endpoint

  defstruct [:name, :pid, :id, :token, :join]

  @doc "Joins `code` as `name`; returns the player, or `{:error, reply}`."
  def join(code, name, extra \\ %{}) do
    test = self()
    # socket/3 must run in the test process; the player process then takes
    # over as the transport, so pushes reach it rather than the test.
    socket = socket(BackendWeb.UserSocket, nil, %{})

    {:ok, pid} =
      Task.start(fn ->
        socket = %{socket | transport_pid: self()}
        payload = Map.merge(%{"name" => name, "client" => "test"}, extra)

        case join(socket, BackendWeb.RoomChannel, "room:" <> code, payload) do
          {:ok, reply, chan} ->
            send(test, {:joined, self(), reply})
            loop(test, name, chan)

          {:error, reply} ->
            send(test, {:join_error, self(), reply})
        end
      end)

    receive do
      {:joined, ^pid, reply} ->
        %__MODULE__{name: name, pid: pid, id: reply.you, token: reply.token, join: reply}

      {:join_error, ^pid, reply} ->
        {:error, reply}
    after
      1000 -> raise "join timed out"
    end
  end

  @doc "Sends `event` from the player and waits for the reply (nil if none)."
  def send_event(%__MODULE__{pid: pid}, event, payload \\ %{}) do
    send(pid, {:send, self(), event, payload})

    receive do
      {:sent, ^pid, reply} -> reply
    after
      1000 -> raise "send timed out"
    end
  end

  @doc "Kills the player's connection, as if the client went away."
  def disconnect(%__MODULE__{pid: pid}) do
    send(pid, :leave)
    ref = Process.monitor(pid)

    receive do
      {:DOWN, ^ref, _, _, _} -> :ok
    end
  end

  defp loop(test, name, chan) do
    receive do
      {:send, from, event, payload} ->
        ref = push(chan, event, payload)

        reply =
          receive do
            %Phoenix.Socket.Reply{ref: ^ref, status: status, payload: p} -> {status, p}
          after
            100 -> nil
          end

        send(from, {:sent, self(), reply})
        loop(test, name, chan)

      :leave ->
        Process.unlink(chan.channel_pid)
        ref = Process.monitor(chan.channel_pid)
        Process.exit(chan.channel_pid, :kill)

        receive do
          {:DOWN, ^ref, _, _, _} -> :ok
        end

      %Phoenix.Socket.Message{event: event, payload: payload} ->
        send(test, {:msg, name, event, payload})
        loop(test, name, chan)

      %Phoenix.Socket.Broadcast{event: event, payload: payload} ->
        send(test, {:msg, name, event, payload})
        loop(test, name, chan)

      _ ->
        loop(test, name, chan)
    end
  end
end

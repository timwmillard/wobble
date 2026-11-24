defmodule BackendWeb.PlayController do
  @moduledoc """
  Serves the WebAssembly client's page at `/` and `/r/:code`. The page reads
  the room code from its own URL; its script and `.wasm` are static files
  under `/play`, built from `client/` (see the client README).
  """
  use BackendWeb, :controller

  def show(conn, _params) do
    path = Application.app_dir(:backend, "priv/static/play/wobble.html")

    if File.exists?(path) do
      conn
      |> put_resp_content_type("text/html")
      |> put_resp_header("cache-control", "no-cache")
      |> send_file(200, path)
    else
      conn
      |> put_resp_content_type("text/plain")
      |> send_resp(503, "The web client isn't built. See client/README.md.")
    end
  end
end

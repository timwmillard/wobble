defmodule BackendWeb.PlayControllerTest do
  use BackendWeb.ConnCase

  @page Application.app_dir(:backend, "priv/static/play/wobble.html")

  setup do
    # The real build is git-ignored; stand in a page unless one is there.
    unless File.exists?(@page) do
      File.mkdir_p!(Path.dirname(@page))
      File.write!(@page, "<!doctype html><title>Wobble</title>")
      on_exit(fn -> File.rm(@page) end)
    end

    :ok
  end

  test "GET / serves the web client", %{conn: conn} do
    assert get(conn, ~p"/") |> html_response(200) =~ "<title>Wobble</title>"
  end

  test "GET /r/:code serves the same page", %{conn: conn} do
    assert get(conn, ~p"/r/abcd") |> html_response(200) =~ "<title>Wobble</title>"
  end
end

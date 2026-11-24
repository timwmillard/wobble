defmodule BackendWeb.DownloadControllerTest do
  use BackendWeb.ConnCase

  @mac_ua "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15"

  test "GET /download links the macOS builds from the latest release", %{conn: conn} do
    html = conn |> get(~p"/download") |> html_response(200)

    assert html =~ "releases/latest/download/Wobble-macOS.dmg"
    assert html =~ "releases/latest/download/Wobble-macOS.zip"
    assert html =~ "Coming soon"
    assert html =~ "git clone https://github.com/timwmillard/wobble.git"
  end

  test "highlights the visitor's platform", %{conn: conn} do
    html =
      conn
      |> put_req_header("user-agent", @mac_ua)
      |> get(~p"/download")
      |> html_response(200)

    assert html =~ "Your system"
  end

  test "highlights nothing on a phone", %{conn: conn} do
    html =
      conn
      |> put_req_header("user-agent", "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X)")
      |> get(~p"/download")
      |> html_response(200)

    refute html =~ "Your system"
  end
end

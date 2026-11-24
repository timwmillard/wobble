defmodule BackendWeb.DownloadControllerTest do
  use BackendWeb.ConnCase

  @mac_ua "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15"

  test "GET /download links the builds from the latest release", %{conn: conn} do
    html = conn |> get(~p"/download") |> html_response(200)

    assert html =~ "releases/latest/download/Wobble-macOS.dmg"
    refute html =~ "Wobble-macOS.zip"
    assert html =~ "releases/latest/download/Wobble-Windows.zip"
    # Linux is still a placeholder.
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

  test "highlights Windows for a Windows browser", %{conn: conn} do
    html =
      conn
      |> put_req_header("user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)")
      |> get(~p"/download")
      |> html_response(200)

    [_, windows_card] = String.split(html, ~s(id="platform-windows"), parts: 2)
    [windows_card | _] = String.split(windows_card, ~s(id="platform-linux"))
    assert windows_card =~ "Your system"
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

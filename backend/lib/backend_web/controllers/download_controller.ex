defmodule BackendWeb.DownloadController do
  @moduledoc """
  The `/download` page: the native apps and how to build from source.

  The files themselves are attached to the latest GitHub release by
  `.github/workflows/release.yml`; their names here must match the ones it
  uploads. A platform with no `files` is shown as coming soon.
  """
  use BackendWeb, :controller

  @platforms [
    %{
      id: :macos,
      name: "macOS",
      detail: "macOS 11 Big Sur or later · Apple Silicon and Intel",
      files: [
        %{name: "Wobble-macOS.dmg", label: "Download .dmg", primary: true}
      ]
    },
    %{
      id: :windows,
      name: "Windows",
      detail: "Windows 10 or later · 64-bit · nothing to install",
      files: [
        %{name: "Wobble-Windows.zip", label: "Download .zip", primary: true}
      ]
    },
    %{
      id: :linux,
      name: "Linux",
      detail: "x86_64 · glibc 2.35+ (Ubuntu 22.04, Debian 12, Fedora 36 or later)",
      files: [
        %{name: "Wobble-Linux.tar.gz", label: "Download .tar.gz", primary: true}
      ]
    }
  ]

  def index(conn, _params) do
    downloads_url = Application.fetch_env!(:backend, :downloads_url)

    platforms =
      Enum.map(@platforms, fn p ->
        files = Enum.map(p.files, &Map.put(&1, :url, "#{downloads_url}/#{&1.name}"))
        %{p | files: files}
      end)

    render(conn, :index,
      page_title: "Download",
      platforms: platforms,
      detected: detect_platform(conn),
      repo_url: Application.fetch_env!(:backend, :repo_url),
      play_url: url(~p"/")
    )
  end

  # A best guess from the User-Agent, only to highlight a card.
  defp detect_platform(conn) do
    ua = conn |> get_req_header("user-agent") |> List.first("")

    cond do
      ua =~ ~r/iPhone|iPad|Android/ -> nil
      ua =~ "Mac OS X" -> :macos
      ua =~ "Windows" -> :windows
      ua =~ "Linux" -> :linux
      true -> nil
    end
  end
end

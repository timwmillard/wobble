defmodule BackendWeb.DownloadHTML do
  @moduledoc "The `/download` page; see `BackendWeb.DownloadController`."
  use BackendWeb, :html

  embed_templates "download_html/*"

  attr :os, :atom, required: true, doc: ":macos, :windows or :linux"
  attr :lines, :list, required: true, doc: "the commands, one per line"
  slot :inner_block, doc: "a note under the platform's name"

  # One platform's commands for a build-from-source step.
  def os_commands(assigns) do
    ~H"""
    <div class="rounded-lg bg-black/40">
      <div class="flex items-center gap-2 border-b border-white/5 px-4 py-2 text-xs font-semibold text-slate-400">
        <.platform_logo id={@os} class="size-3.5" />
        {os_name(@os)}
        <span :if={@inner_block != []} class="font-normal text-slate-500">
          · {render_slot(@inner_block)}
        </span>
      </div>
      <pre class="overflow-x-auto px-4 py-3 text-[13px] text-slate-200"><code>{Enum.join(@lines, "\n")}</code></pre>
    </div>
    """
  end

  defp os_name(:macos), do: "macOS"
  defp os_name(:windows), do: "Windows"
  defp os_name(:linux), do: "Linux"

  attr :class, :string, default: "size-6"
  attr :id, :atom, required: true

  # Platform logos, drawn in currentColor.
  def platform_logo(%{id: :macos} = assigns) do
    ~H"""
    <svg viewBox="0 0 24 24" class={@class} fill="currentColor" aria-hidden="true">
      <path d="M16.37 12.73c-.02-2.3 1.88-3.4 1.96-3.46-1.07-1.56-2.73-1.78-3.32-1.8-1.41-.14-2.76.83-3.47.83-.72 0-1.82-.81-3-.79-1.54.02-2.96.9-3.76 2.28-1.6 2.78-.41 6.9 1.15 9.16.76 1.1 1.67 2.34 2.86 2.3 1.15-.05 1.58-.74 2.97-.74 1.38 0 1.77.74 2.98.72 1.23-.02 2.01-1.12 2.76-2.23.87-1.28 1.23-2.51 1.25-2.58-.03-.01-2.4-.92-2.38-3.69ZM14.1 5.96c.63-.77 1.06-1.83.94-2.89-.91.04-2.01.61-2.66 1.37-.58.67-1.1 1.75-.96 2.79 1.01.08 2.05-.52 2.68-1.27Z" />
    </svg>
    """
  end

  def platform_logo(%{id: :windows} = assigns) do
    ~H"""
    <svg viewBox="0 0 24 24" class={@class} fill="currentColor" aria-hidden="true">
      <path d="M3 5.1 10.4 4v7.1H3V5.1Zm0 13.8 7.4 1.1v-7H3v5.9Zm8.3 1.2L21 21.5V13h-9.7v7.1Zm0-16.2V11H21V2.5L11.3 3.9Z" />
    </svg>
    """
  end

  def platform_logo(%{id: :linux} = assigns) do
    ~H"""
    <svg
      viewBox="0 0 24 24"
      class={@class}
      fill="none"
      stroke="currentColor"
      stroke-width="1.6"
      stroke-linecap="round"
      stroke-linejoin="round"
      aria-hidden="true"
    >
      <rect x="3" y="4" width="18" height="16" rx="2.5" />
      <path d="m7 10 3 2.5L7 15M12.5 15H17" />
    </svg>
    """
  end
end

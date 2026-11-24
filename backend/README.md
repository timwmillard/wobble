# Backend

To start your Phoenix server:

* Run `mix setup` to install and setup dependencies
* Start Phoenix endpoint with `mix phx.server` or inside IEx with `iex -S mix phx.server`

Now you can visit [`localhost:4000`](http://localhost:4000) from your browser.

Ready to run in production? Please [check our deployment guides](https://phoenix.hexdocs.pm/deployment.html).

## Learn more

* Official website: https://www.phoenixframework.org/
* Guides: https://phoenix.hexdocs.pm/overview.html
* Docs: https://phoenix.hexdocs.pm
* Forum: https://elixirforum.com/c/phoenix-forum
* Source: https://github.com/phoenixframework/phoenix

## Deploying to Fly.io

`fly.toml` and the `Dockerfile` live at the repo root, because the image
builds both the WebAssembly client (`client/`, with emsdk) and this release.
From the repo root:

```sh
fly apps create wobble          # once; pick another name if taken, and update
                                # app + PHX_HOST in fly.toml to match
fly secrets set SECRET_KEY_BASE=$(openssl rand -base64 48)  # once
fly deploy --ha=false           # one machine: rooms live in memory
```

Native clients connect with `--server wss://wobble.fly.dev`.

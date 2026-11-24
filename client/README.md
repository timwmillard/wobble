# Wobble client

Client for Wobble, built on [Raven](https://oscar-soirey.github.io/Raven/):
native (macOS, Linux, Windows) and in the browser (WebAssembly).
The native build needs only a C11 compiler and CMake.

```sh
cmake -B build && cmake --build build
./build/wobble                                   # connect screen
./build/wobble --server ws://localhost:4000 --room abcd --name Ana
ctest --test-dir build                           # core tests
```

Bots, for load tests and for playing along with people:

```sh
./build/wobble_bot -s ws://localhost:4000 -r bots -n 8      # 8 bots play a game
./build/wobble_bot -s ws://localhost:4000 -r abcd -n 2 -p 3 # 2 bots wait for you
```

Start the server with `WOBBLE_FAST=1 mix phx.server` (in `../backend`) for
short timers. `WOBBLE_CAPTURE=out.ppm WOBBLE_CAPTURE_AFTER=3` (or `choosing` /
`drawing`) saves a screenshot of the app and quits.

## Web build

The same app in the browser, with the browser's WebSocket in place of webc
(`src/transport_web.c`). Needs [Emscripten](https://emscripten.org) (Raven
is tested with 7.0.0; `brew install emscripten` works).

```sh
make web    # emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web
```

It copies `wobble.html`, `.js` and `.wasm` into `../backend/priv/static/play`
(git-ignored; set `-DWOBBLE_WEB_DEPLOY_DIR=` to skip). Phoenix serves the
page at `/` and `/r/<code>`: the room comes from the link, the server is the
page's own host, and `?name=Ana` fills the name, so
`http://localhost:4000/r/abcd?name=Ana` joins straight away.

The web build uses the committed `src/gui_xml.h` (it can't run `embedc`), so
build natively once after editing `gui.xml`.

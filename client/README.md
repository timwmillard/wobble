# Wobble client

Client for Wobble, built on [Raven](https://oscar-soirey.github.io/Raven/):
native (macOS, Linux, Windows) and in the browser (WebAssembly).
The native build needs only a C11 compiler and CMake.

```sh
cmake -B build && cmake --build build
./build/wobble                                   # connect screen
./build/wobble --server ws://localhost:4000 --room abcd --name Ana
./build/wobble https://wobblegame.fly.dev/r/abcd # a room link
ctest --test-dir build                           # core tests
make release                                     # build-release/, prod server
make dmg                                         # macOS: Wobble-macOS.dmg
make windows                                     # Windows: Wobble-Windows.zip
```

`make dmg` is a universal (arm64 + x86_64) release build, ad-hoc signed and
packaged by CPack into `build-release/Wobble-macOS.dmg`.

`make windows` cross-compiles from macOS or Linux with MinGW-w64
(`brew install mingw-w64`, or `apt install gcc-mingw-w64-x86-64`;
toolchain in `cmake/mingw-w64.cmake`) into `build-windows/Wobble-Windows.zip`:
one static `wobble.exe` with its icon (`release/windows/Wobble.ico`, made from
the macOS icon SVGs). Raven needs the Vulkan headers for Windows: pass
`VULKAN_HEADERS=<dir containing vulkan/vulkan.h>`, or Raven downloads them.
On Windows itself, plain `cmake -B build && cmake --build build` with Visual
C++ works too.

On Linux, `cpack` in a release build directory makes `Wobble-Linux.tar.gz`:
a `wobble/` folder with the binary, `wobble.desktop` and the icon. Only libX11
is linked; libssl, Vulkan, Wayland and libXi are opened at run time. Build it
on an old distro, since it runs on that glibc and newer (the release uses
Ubuntu 22.04).

Pushing a `v*` tag runs `.github/workflows/release.yml`, which builds all
three (macOS on a macOS runner, Windows cross-compiled and then tested on a
Windows runner, Linux in an Ubuntu 22.04 container). Only when all of them
pass does it create the GitHub release with the three files, so a release
never has a platform missing; the server's `/download` page links to the
latest release's copies.

## Room links

A room's link, `https://<host>/r/<code>`, is the one thing to share. Clicked,
it opens the web client in that room. Pasted into the desktop app's room
field, given as an argument or found on the clipboard at start-up, it sets
both the server and the room. In a room, "Copy link" copies it, on desktop
and on the web. See `src/link.h` for everything the room field accepts.

A bare code joins the default server: `--server`, else `$WOBBLE_SERVER`,
else the one the build was configured with. That's `ws://localhost:4000`
unless you set `-DWOBBLE_SERVER=...`. `make release` sets it to
`wss://wobblegame.fly.dev` (`make release RELEASE_SERVER=...` to change
it). "Custom server" on the connect screen shows the field.

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
`http://localhost:4000/r/abcd?name=Ana` joins straight away. Once in a room
the address bar shows its link (`/r/<code>`, without `?name=`), ready to
share. Browser-side helpers the C code calls live in `src/web/wobble_lib.js`.

The web build uses the committed `src/gui_xml.h` (it can't run `embedc`), so
build natively once after editing `gui.xml`.

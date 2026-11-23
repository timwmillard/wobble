# Wobble client

Native client for Wobble, built on [Raven](https://oscar-soirey.github.io/Raven/).
Needs only a C11 compiler and CMake.

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

# Scribble: Raven + Phoenix drawing game — build plan

Nov 23, 2026 · @Tim Millard

## Summary

We are building a Pictionary-style party game. Players join a room. They take turns as the drawer: the drawer picks one of three words and draws it, and everyone else races to guess it in chat. A player is in a room from a link in the browser or from a native desktop app, and both kinds of client play together in the same room.

**Stack decisions**

| Layer | Choice | Why |
| --- | --- | --- |
| Backend | Elixir + Phoenix (Channels, Presence) | One supervised process per room owns the game; WebSocket broadcast comes built in; it scales to thousands of rooms on one small box |
| Database | None for now | Live game state stays in memory in the room processes and the word list is a file compiled in, so nothing needs to outlast a game yet. Ecto was removed until something does (stats, accounts, custom word lists); then add Ecto on SQLite (`ecto_sqlite3`) with the file on a Fly volume, or Postgres if we need more than one node |
| Client UI | [Raven](https://oscar-soirey.github.io/Raven/) (C11) | One C codebase targets Windows, Linux and macOS (Vulkan) and the browser (WebAssembly + WebGL 2); supports pen input and custom drawing |
| Client networking | Our existing C network library, **webc** (copied from `chat/src/net` into `client/src/net`) | Native: HTTP/1.1 + WebSocket (RFC 6455) + SSE on the OS's TLS, so `wss://` works today. Web: needs a browser-WebSocket transport (see Risks) |
| JSON | Our own `json.c` / `json.h` (copied from `chat/src`) | Arena-backed parser (`json_parse`, `json_get`, `json_at`, `json_str`) plus `json_write_string` for output; plain C11, no extra dependency |
| Web hosting | Phoenix serves the WASM build | Link `game.example/r/abcd` opens straight into the room |

**Guiding rules for whoever builds this**

- The server is in charge of the game. Clients only send what the player did and draw what the server says. The secret word is never sent to a guesser.
- The Phoenix wire protocol is the contract. Any client that speaks it can play.
- Raven sits behind a thin `ui_*` layer, because its API is pre-1.0 and may change.

## Architecture

The client has three layers around a shared C core, and only the transport and UI layers differ between the desktop and web builds. On the server, each room is a single process that holds the truth.

&#91;embedded content: system architecture · client layers and server processes\]

A player's action flows like this: the UI calls the core, the core writes a channel frame, and the transport sends it. On the server, the RoomChannel checks the sender's role and the phase, and the Room process updates the game and broadcasts the result. Incoming frames take the same path in reverse, and the UI redraws whatever the core marks as changed.

## Game rules and turn flow

A game is 3 rounds; in each round every player draws once. All timers run on the server.

&#91;embedded content: room phases · 5 states, 1 decision\]

If a room drops below 2 players mid-game, it returns to Lobby. If the drawer leaves, the turn jumps straight to Reveal.

| Rule | Default | Notes |
| --- | --- | --- |
| Players per room | 2–12 | Joining a full room is rejected |
| Rounds | 3 | Configurable per room later |
| Choose time | 15 s | When it runs out, the server picks the first word |
| Draw time | 80 s | The turn ends early when every guesser is correct |
| Hint reveals | at 50% and 75% of draw time | One random letter each; never the last hidden letter |
| Reveal time | 5 s | Then the next drawer is chosen |
| Guesser points | 50 + 250 × (time left ÷ draw time) | Rounded; faster guesses earn more |
| Drawer points | 25 per correct guesser | Rewards drawable drawings |
| Close guess | edit distance 1, word length 4 or more | Only the guesser sees "You're close!" |

Guess matching ignores case and extra whitespace. After guessing correctly, a player's chat goes only to the drawer and to other players who have guessed, so the answer can't leak.

## Wire protocol

Clients speak the standard Phoenix Channels V2 JSON format over one WebSocket. Every client, native or web, uses exactly these messages.

**Connection and framing**

- URL: `ws(s)://<host>/socket/websocket?vsn=2.0.0`
- Every frame is a JSON array: `[join_ref, ref, topic, event, payload]`
- Join: `["1","1","room:abcd","phx_join",{"name":"Ana","client":"raven-native/0.1"}]`
- Replies arrive as event `phx_reply` with payload `{"status":"ok"|"error","response":{...}}`, matched by `ref`
- Heartbeat every 30 s: `[null,"<ref>","phoenix","heartbeat",{}]`. The server drops a socket after 60 s of silence.
- Room codes match `^[a-z0-9-]{1,24}$`. Names are 1–16 printable characters, trimmed by the server.

**Client → server**

| Event | Payload | Who may send | Server action |
| --- | --- | --- | --- |
| `phx_join` | `{name, client, token?}` | anyone | Adds the player, or gives back the seat that `token` names; replies `{you, token, state, strokes}` |
| `start` | `{}` | host, in Lobby or Game over | Starts a new game |
| `choose` | `{index: 0..2}` | drawer, in Choosing | Locks in the word |
| `stroke` | `{id, pts:[x,y,...], c, s, end, p?}` | drawer, in Drawing | Saves it, then sends it to everyone else. No reply unless it's rejected |
| `undo` | `{id}` | drawer, in Drawing | Removes that stroke for everyone |
| `clear` | `{}` | drawer, in Drawing | Wipes the canvas for everyone |
| `guess` | `{text}` (max 100 chars) | anyone | Checks it against the word, or sends it as chat |

**Server → client**

| Event | To | Payload |
| --- | --- | --- |
| `state` | everyone | `{phase, turn, round, rounds, players:[{id,name,score,guessed,drawing,connected}], host, drawer, hint, time_left_ms, word?}` (`word` only in Reveal and Game over) |
| `choices` | drawer only | `{words:[w1,w2,w3], time_left_ms}` |
| `word` | drawer, and each guesser once correct | `{word}` |
| `stroke` | everyone except the drawer | same payload the drawer sent |
| `undo` / `clear` | everyone except the drawer | `{id}` / `{}` |
| `chat` | everyone, or only guessed players and the drawer | `{from, from_id, text, kind: msg\|system\|correct\|secret}` (`from` is null for system lines) |
| `close` | that guesser only | `{text}` |

**Stroke encoding**

- Each chunk after a stroke's first repeats the previous chunk's last point, so every chunk draws on its own and joins on to the last; receivers just append.
- The canvas is a fixed 800×600 coordinate space. Clients scale it to fit their window.
- One stroke = one pen-down-to-pen-up line. The drawer sends it in chunks about every 33 ms. All chunks share an `id`; `end:true` marks pen-up.
- `c` = palette index 0–15, `s` = brush size 1–40. Coordinates are integers. Pen pressure can come later as optional `p:[...]` values (0–255).
- Version 1 sends JSON arrays. If bandwidth becomes a problem, switch to base64 delta-encoded int16; nothing else changes.

## Backend design (Phoenix)

One GenServer per room holds the whole game state. Channels stay thin: they check message shapes and forward to the room.

Rejected events reply `{status: "error", response: {reason}}` with reasons such as `not_host`, `not_drawer`, `wrong_phase`, `not_enough_players`, `room_full`, `invalid`, `rate_limited`, `too_much_ink`. Rate limits (strokes 60/s, chat 5 per 3 s) are kept per channel. For local testing, `WOBBLE_FAST=1 mix phx.server` shortens the timers (2 s choose, 8 s draw, 0.5 s reveal).

**Modules**

| Module | Job |
| --- | --- |
| `Backend.Application` | Starts `Registry` (`Backend.RoomRegistry`), `DynamicSupervisor` (`Backend.RoomSupervisor`), `Phoenix.PubSub`, and the Endpoint |
| `Backend.Room` | GenServer, `restart: :temporary`, registered as `{:via, Registry, {RoomRegistry, code}}`. Owns state, timers and scoring |
| `Backend.Rules` | Pure functions: `next_drawer/1`, `score/2`, `hint/2`, `close_guess?/2`, `normalize/1`. Unit-tested without processes |
| `Backend.Words` | Word list (start with 300–500 easy nouns in `priv/words/en.txt`); `pick(3, exclude: used)` |
| `BackendWeb.UserSocket` | `channel "room:*", RoomChannel`; assigns a random `player_id` on connect |
| `BackendWeb.RoomChannel` | Joins, validates payloads, casts to the room, relays private `{:push, event, payload}` messages |
| ~~`BackendWeb.Presence`~~ | Not needed: the Room monitors each channel pid, and that drives `connected` |
| `BackendWeb.PageController` | Serves the WASM client at `/` and `/r/:code`, with COOP/COEP headers if threads are used |

**Room state**

```elixir
%Room{
  code: "abcd", phase: :lobby, turn: 0, round: 0, rounds: 3,
  players: %{id => %{name, score, guessed, pid, mref}},
  order: [id], host: id, drawer: id | nil, drawn_this_round: MapSet,
  choices: [w1, w2, w3], word: nil, revealed: MapSet,
  deadline_ms: nil, strokes: [stroke] (newest first), stroke_bytes: 0,
  used_words: MapSet, empty_since: nil
}
```

**Rules for the room process**

- **Timers.** Every timer carries the current `turn` (for example `{:turn_timeout, turn}`), and a handler ignores any timer whose turn number is old. That makes cancelling timers unnecessary.
- **Time left.** Send `time_left_ms` computed from `deadline_ms` with `System.monotonic_time`. Clients count down on their own and correct themselves on each `state` message.
- **Leaving.** The room monitors each channel pid. When one goes `:DOWN`, keep the player for 30 s marked `connected:false`, so a reconnect with the same token keeps their score. After that, remove them. Reassign host to the next player in order. Stop the room after 5 minutes empty.
- **Broadcasts.** Public events go out with `Endpoint.broadcast`. Strokes use `broadcast_from(drawer_pid, ...)` so they aren't echoed back. Private events go out as `send(pid, {:push, ...})`.

**Validation and anti-cheat**

- Reject every event that isn't allowed for the sender's role and the current phase (see the protocol table).
- Strokes: at most 256 points per chunk. Coordinates are clamped to 0–800 and 0–600, and `c` and `s` are checked against their ranges. Cap each turn at 2 MB of stroke data and each drawer at about 60 messages per second.
- Chat: at most 100 characters and 5 messages per 3 s per player. Messages that contain the word are blocked for the drawer and for players who have already guessed.
- The word is only ever sent to the drawer, to guessers who got it right, and to everyone at Reveal.

**Config**

All timings and limits live in `config :backend, :rules` so tests can shorten them. Read `PORT`, `PHX_HOST` and `SECRET_KEY_BASE` in `runtime.exs`. Native clients send no `Origin` header. Keep `check_origin` set to your web host, which only applies to browsers.

## Client design (C + Raven)

The client has three layers. Only the bottom two touch platform APIs, and only `ui/` touches Raven.

1. **core (`src/phx.c`, `src/game.c`)** — plain C11, no I/O. It holds the Phoenix channel state (refs, join, heartbeat timing), a copy of the game state, the stroke list and the chat log. It's driven by `core_on_message(json)` and `core_tick(now_ms)`, and it emits outgoing frames through a `net_send` callback.
2. **`net/`** — the transport. `net/` is webc, copied as-is from `chat/src/net` (see *Networking and JSON* below). `transport_native.c` wraps its `ws_*` API; `transport_web.c` uses `emscripten/websocket.h`. Both implement the same `transport_*` interface.
3. **ui (`src/main.c`, `src/canvas.c`, `src/gui.xml`)** — Raven. It holds the XML layouts, the canvas widget and the event handlers. It reads core state and calls core actions; nothing else.

**Core API (sketch)**

```c
typedef struct Game Game;
typedef void (*SendFn)(const char *frame, size_t len, void *ud);

Game *game_new(SendFn send, void *ud);
void  game_connected(Game*, const char *room, const char *name);
void  game_on_message(Game*, const char *json, size_t len); // from net
void  game_tick(Game*, uint64_t now_ms);                     // heartbeat, timers

// player actions
void game_start(Game*);
void game_choose(Game*, int index);
void game_pen_down(Game*, float x, float y, float pressure);
void game_pen_move(Game*, float x, float y, float pressure);
void game_pen_up(Game*);
void game_undo(Game*);  void game_clear(Game*);
void game_guess(Game*, const char *text);

// read-only views for the UI
const GameState *game_state(const Game*);
const Stroke    *game_strokes(const Game*, size_t *count);
uint32_t         game_dirty(Game*); // bitmask: PLAYERS|CHAT|CANVAS|PHASE
```

The network layer and the UI may run on different threads. Push incoming frames into a lock-free queue (or Raven's job system) and process them on the UI thread, before `game_tick`. Only the UI thread touches `Game`.

**Screens (Raven XML)**

| Screen | Contents |
| --- | --- |
| `connect.xml` | Name, room code, server (prefilled from the URL on web), Join button, error label |
| `game.xml` | Top bar: round, hint or secret word, countdown. Centre: canvas. Right dock: player list with scores and a ✓ for guessed; chat log; guess input. Bottom bar (drawer only): 16-colour palette, 4 brush sizes, eraser, undo, clear |
| Overlays in `game.xml` | Lobby (player count, Start button for host), word-choice dialog (3 buttons + countdown), reveal ("The word was…"), game over (ranked scores, Play again for host) |

**Canvas widget**

- Raven API, as used in `client/src/canvas.c`: an `R_WIDGET_CUSTOM` with `Rcustom_set_paint_callback` (draw with `Rdraw_line` / `Rdraw_circle` / `Rdraw_rect`) and `Rwidget_set_pen_callback` for input (`RPenEvent`: `phase` down/move/up/hover/leave, `local_x/y`, `pressure`, `eraser`). `Rwidget_set_pen_mouse(..., R_TRUE)` so the mouse draws too; `Rwidget_invalidate` to repaint.
- Map the 800×600 game space into the widget with letterboxing. Convert pointer and pen events back into game space before calling `game_pen_*`.
- *(Not done yet: v1 repaints every stroke each frame, which is fine for a turn's worth of ink.)* Keep a cached layer (texture) of finished strokes, and draw only the stroke in progress live. Redraw the cache from the stroke list on `undo`, `clear` or resize.
- Draw each segment as a thick line with round caps, at the stroke's size. Use pen pressure as width when it's available.
- Thin out input locally: skip points less than 1.5 px from the last one, and send a chunk every 33 ms or every 64 points.
- Draw the local stroke immediately, without waiting for the server. Because the drawer's own strokes aren't echoed back, there's nothing to reconcile.

**Web build specifics**

- Build with Emscripten through Raven's CMake web target. Output `index.html`, `.js` and `.wasm` into Phoenix's `priv/static/play/`.
- The page reads the room code from `/r/:code` and the server from `location.host` (`wss` when the page is served over https).
- Fit the canvas to the viewport and handle `devicePixelRatio`. Map touch and pen pointer events to `game_pen_*` so phones and tablets work.
- If the build needs pthreads, Phoenix must send `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` on the play page.

## Networking and JSON (client)

Both come from the `chat` project (`~/dev/x/chat`), which already ships a Raven app talking to a streaming HTTP API.

**webc (`client/src/net/`)** — copied verbatim from `chat/src/net`. Public API is `net.h` only; everything else is internal.

- `ws_connect(&(ws_desc){.url, .headers, .on_open, .on_message, .on_close, .on_error, .user_data})` starts a connection on its own network thread and returns at once. `ws_send_text` / `ws_close` are thread-safe; `ws_free` can be called from anywhere and guarantees no callback runs afterwards.
- Backends: Network.framework on Apple, POSIX sockets + system libssl (loaded at runtime) on other Unix, Winsock + SChannel on Windows. So `ws://` and `wss://` both work natively with no extra dependency. Link `-framework Network -framework Security` (Apple), `ws2_32 secur32 bcrypt` (Windows), `dl pthread` (other).
- webc sends its own WebSocket pings (`ping_interval_ms`, default 30 s). Those keep the TCP connection alive but are **not** Phoenix heartbeats; the core still sends `["null","ref","phoenix","heartbeat",{}]` every 30 s.
- CMake: the same `net_library()` function as in chat builds `src/net/*.c` into a static `net` library.

**Threading.** webc calls back on its network thread. As in chat's `main.c`, each callback copies the frame into a heap event and hands it to the UI thread with `Rtask_post(fn, ev)`. Only the UI thread touches `Game`. The bot (no Raven) uses a small mutex-protected queue instead.

**JSON (`client/src/json.c`, `json.h`)** — copied from `chat/src`. Uses `base.h` arenas and `string` slices.

- Parse: `json *j = json_parse(&arena, text)` (NULL if invalid), then `json_get(j, S("key"))`, `json_at(j, i)`, `json_str(j)`; `j->type`, `j->number`, `j->boolean`, `j->len`. All accessors are NULL-safe, so lookups chain.
- Write: frames are built with a `string_builder` and `json_write_string(&sb, s)` for every string, so escaping is always right.
- Each incoming frame is parsed into a scratch arena that is reset after the frame is applied to `Game`; anything kept (names, chat, strokes) is copied into the game's own storage.

**Phoenix frames on top.** `phx.c` builds `[join_ref, ref, topic, event, payload]` arrays and parses incoming ones into `{join_ref, ref, topic, event, payload json*}`. It owns ref counters and heartbeat timing; `game.c` owns everything game-specific.

## Milestones

There are five milestones, each ending in something you can demo. M0 decides whether Raven stays; if it fails, only `ui/` is swapped out.

&#91;embedded content: milestones · 5 phases, 3 gates (not to scale)\]

### M0 — Risk spike (throwaway code)

- [ ] Raven window with a custom canvas: mouse and pen input, thick round-capped lines, a cached layer for finished strokes
- [ ] The same app built with Emscripten and opened in Chrome, Safari and Firefox
- [ ] Our network library talking to a stock Phoenix echo channel (native); an `emscripten/websocket.h` version doing the same (web)
- [ ] Write down: does the web build need pthreads? Does pen pressure arrive in the browser? What's the frame time with 5,000 segments?

**Done when:** strokes drawn in one client appear in another through the echo channel, on both native and web. Go/no-go on Raven is recorded with the measurements.

**Web spike (Nov 24):** the real app (not throwaway code), built with Emscripten 7.0.1, joined a room from `/r/spike?name=Web` in Chrome against three native bots. It received strokes and chat, sent a guess, and drew a stroke that the bots received. No pthreads or COOP/COEP needed. Download: `wobble.wasm` 1.7 MB (709 KB gzipped) + `wobble.js` 37 KB gzipped. Still open: Safari and Firefox, a real stylus, a phone, frame time with 5,000 segments.

### M1 — Protocol and server

- [x] Phoenix app in `backend/` (Ecto later removed, see Database); add `Rules`, `Room`, `RoomChannel` and the word list (Presence dropped, see Backend design)
- [x] The full turn flow with timers, hints, scoring, and leaving or rejoining (rejoin by token pulled forward from M4)
- [x] C core (`client/src/phx.c`, `game.c`) with a fake-transport test harness (`client/tests/game_test.c`)
- [x] Bots: `client/src/bot.c` (`wobble_bot`) on the same core and transport as the app; bots in one process share the drawer's word so some guesses are right

**Status (Nov 23):** done locally — 8 bots finished a 3-round game against `WOBBLE_FAST=1 mix phx.server` with no server errors and the word-leak check on; 26 ExUnit tests and the core tests pass. Not yet wired into CI.

**Done when:** 8 bots finish a 3-round game in CI. The ExUnit and core tests pass. No guesser ever receives the word before guessing it (tested).

### M2 — Playable native client

- [x] Raven screens: connect, lobby, game, word choice, reveal, game over (`client/src/gui.xml`)
- [x] Palette, brush sizes, eraser, undo, clear; chat with correct, close and secret styling
- [x] Countdown, hint display, player list with scores and a guessed mark

**Status (Nov 23):** built and seen working against bots on macOS (screenshots of connect, guessing, word choice and drawing). Mouse and pen drawing in the app have not been exercised by a person yet; Linux and Windows builds untried.

**Done when:** four people on three operating systems play a full game against a hosted server without crashes.

### M3 — Web client

- [ ] `transport_web.c`; WASM build output in `priv/static/play`; routes `/` and `/r/:code`
- [ ] Responsive layout for phone portrait (canvas on top, chat below); touch and stylus input
- [ ] COOP/COEP headers if M0 showed they're needed

**Done when:** a room with native and web players (including one phone) plays a full game. Load time is under 3 s on broadband.

### M4 — Launch hardening

- [ ] TLS: Caddy in front of Phoenix; native client on `wss://`
- [ ] Reconnect with a session token within 30 s, keeping the score
- [ ] Rate limits, vote-kick, client version check
- [ ] CI release builds (signed macOS, Windows zip, Linux AppImage); Docker deploy

**Done when:** a public playtest with 20+ concurrent players runs for an hour with no server restarts.

## Risks and open questions

The biggest unknowns are all on the client side, and Milestone 0 exists to settle them before any game logic is written.

| Risk | Impact | Mitigation / check |
| --- | --- | --- |
| Raven is pre-1.0 (the site says v0.1, the README says v0.2.0); its API may change | Rework of the UI layer | Pin a Raven commit. Keep all Raven calls inside `ui/`. Budget a day per Raven upgrade |
| Our network library has no browser transport | No web client | **Resolved (Nov 24 spike):** `transport_web.c` on `emscripten/websocket.h` behind the same `transport_*` interface; a Chrome tab played against native bots |
| Raven's web build may need pthreads | Hosting must send COOP/COEP headers, which can break embeds | **Resolved:** not needed. Raven's browser build runs on the main thread (`Rweb_start`), so no COOP/COEP headers |
| Pen pressure and stylus may not work in Raven's web build | Only flat-width strokes on web | Raven forwards PointerEvent `pointerType` and `pressure` to the same pen events (`emscripten_platform.c`); still to try with a real stylus in Chrome and Safari |
| Custom canvas drawing may be slow in Raven with thousands of segments | Lag on long drawings | Use the cached finished-stroke layer. Cap strokes per turn |
| Vulkan is unavailable on some machines (old GPUs, VMs) | Native app won't start | Point those users to the web client. Show a clear error at startup |
| Native TLS (`wss://`) | Plain `ws://` is blocked on some networks | **Resolved:** webc already speaks `wss://` on the OS's TLS (Network.framework / SChannel / system libssl loaded at runtime). Caddy still terminates TLS in front of Phoenix |
| Abuse: offensive drawings or chat, spam rooms | Bad experience in public rooms | Rooms are private by code at first. Add a vote-kick and rate limits; public matchmaking comes later |

**Open questions for the owner**

- [x] Does our network library already support WebSocket framing and TLS, or only raw TCP? **Yes, both** — webc has a full RFC 6455 client and uses the OS's TLS.
- [ ] Public matchmaking, or private rooms by code only (recommended for v1)?
- [ ] Custom word lists per room, and which languages at launch?
- [ ] Accounts and persistent stats, or anonymous only (recommended for v1)?
- [ ] Mobile: is the web build enough, or will Raven's experimental iOS and Android targets be needed?

## Repo layout, deployment and testing

Use one repo with the server and client side by side. CI builds all three targets on every push.

```text
wobble/
  SCRIBBLE_PLAN.md
  backend/                # Phoenix app `backend` (Backend / BackendWeb); no database
    lib/backend/          # room.ex, rules.ex, words.ex, application.ex
    lib/backend_web/      # endpoint, channels/user_socket, channels/room_channel,
                          # presence, page_controller
    priv/words/en.txt
    priv/static/play/     # WASM build output (copied by CI)
    test/
  client/                 # follows client/CLAUDE.md: C11 + CMake only, deps vendored
    CMakeLists.txt        # targets: wobble (Raven UI), wobble_bot, tests
    deps.lua / deps/      # base.h, Raven (cdeps)
    tool/embedc/          # embeds src/gui.xml
    src/net/              # webc, copied from chat/src/net (do not fork casually)
    src/json.c json.h     # copied from chat/src
    src/phx.c phx.h       # Phoenix Channels V2 framing: refs, join, heartbeat
    src/game.c game.h     # game core: state, strokes, chat; no I/O, no Raven
    src/transport.h       # connect / send / close; callbacks on a network thread
    src/transport_native.c  # on webc ws_*   (transport_web.c later, M3)
    src/main.c canvas.c gui.xml  # Raven UI
    src/bot.c             # headless bot on the same core + transport
    tests/                # core tests with a fake transport
```

**Deployment**

- Server: a Phoenix release in a Docker image on Fly.io, Render or a small VPS. A single node is enough for v1. There is no database yet; when one arrives, a SQLite file on a persistent volume is enough for one node. Room state lives in memory, so a redeploy ends running games; announce deploys or drain first.
- TLS: Caddy or the platform's proxy terminates HTTPS and WSS.
- Web client: served by Phoenix from `priv/static/play`. Cache the `.wasm` file with a content hash in its name.
- Native client: GitHub Releases with zips for Windows, Linux (AppImage) and macOS (signed and notarized `.app` with MoltenVK bundled). The client checks the server's `client` version on join and shows "please update" when it's too old.
- Scaling later: use `Horde` or `:pg` for a room registry across nodes. Route by room code so one room stays on one node.

**Testing**

| Layer | How | What it proves |
| --- | --- | --- |
| `Backend.Rules` | ExUnit, pure functions | Scoring, hints, close-guess matching, drawer rotation |
| `Backend.Room` | ExUnit with timings shortened to milliseconds | Full game played by fake player pids, leave and rejoin, drawer leaving mid-turn |
| `RoomChannel` | `Phoenix.ChannelTest` | Role and phase checks, private pushes, the word never reaching a guesser |
| C core | Unit tests with a fake `SendFn` and recorded server frames | Parsing, state updates, stroke chunking |
| End to end | 8 bots in one room for 3 rounds, in CI | Protocol compatibility; no crashes or leaks |
| Manual | Native + web in the same room, mouse and stylus | Feel, latency, layout on phone and desktop |

/* game.h — the client's game core: the Phoenix channel (join, refs,
 * heartbeat), a copy of the server's game state, the strokes and the chat.
 *
 * Plain C, no I/O and no threads: the transport feeds it frames with
 * game_on_message and it writes frames through the send callback. Call
 * everything from one thread.
 */

#ifndef GAME_H
#define GAME_H

#include "base.h"

// The fixed drawing space every client scales to fit.
#define GAME_CANVAS_W 800
#define GAME_CANVAS_H 600

#define GAME_MAX_PLAYERS 12
#define GAME_PALETTE_SIZE 16
#define GAME_CHAT_LINES 128
#define GAME_HEARTBEAT_MS 30000

// Send stroke chunks this often while the pen is down, or sooner when this
// many points are waiting.
#define GAME_CHUNK_MS 33
#define GAME_CHUNK_POINTS 64

typedef enum {
   GAME_OFFLINE,    // not connected yet, or the connection is gone
   GAME_JOINING,    // connected, waiting for the join reply
   GAME_LOBBY,
   GAME_CHOOSING,
   GAME_DRAWING,
   GAME_REVEAL,
   GAME_OVER,
} game_phase;

// Bits returned by game_dirty: what changed since the last call.
enum {
   GAME_DIRTY_PHASE = 1 << 0,   // phase, round, hint, word, host, drawer
   GAME_DIRTY_PLAYERS = 1 << 1, // the player list or scores
   GAME_DIRTY_CHAT = 1 << 2,    // new chat lines
   GAME_DIRTY_CANVAS = 1 << 3,  // strokes
   GAME_DIRTY_CHOICES = 1 << 4, // the word choices (drawer only)
   GAME_DIRTY_ERROR = 1 << 5,   // error text
};

typedef struct {
   string id;
   string name; // up to 16 characters
   int score;
   bool guessed, drawing, connected;
} game_player;

typedef enum {
   GAME_CHAT_MSG,     // someone's guess or message
   GAME_CHAT_SYSTEM,  // from the server
   GAME_CHAT_CORRECT, // someone guessed the word
   GAME_CHAT_SECRET,  // only for players who know the word
   GAME_CHAT_CLOSE,   // "close!" — only this player sees it
} game_chat_kind;

typedef struct {
   game_chat_kind kind;
   char from[64]; // empty for system lines
   char text[416];
} game_chat_line;

typedef struct {
   i16 x, y;
   u8 pressure; // 0-255; 255 when the device has none
} game_point;

typedef LIST(game_point) game_point_list;

typedef struct {
   u32 id;
   u8 color; // palette index
   u8 size;  // 1-40
   bool done;
   game_point_list pts;
} game_stroke;

/* The strings in game_state are owned by the game and stay valid only until
 * the next call into it that can change the state (game_on_message,
 * game_connected, game_disconnected); copy any you want to keep. Each is
 * NUL-terminated, so .data can go where a C string is needed, and never has
 * a NULL .data: unset fields are "". */
typedef struct {
   game_phase phase;
   int turn, round, rounds;
   game_player players[GAME_MAX_PLAYERS];
   int player_count;
   string you;   // our player id
   string token; // presented on rejoin to keep our seat
   string host;
   string drawer;
   string hint; // "_ _ _ e" style, for guessers
   string word; // the word, once we may know it
   string choices[3];
   bool has_choices;
   u64 deadline_ms; // local clock; 0 = no timer
   string error;
} game_state;

typedef void (*game_send_fn)(const char *frame, usize len, void *user_data);

typedef struct game game;

game *game_new(game_send_fn send, void *user_data);
void game_free(game *g);

// The transport is open: join room (its code, e.g. "abcd") as name. token
// may be empty; pass the one from a previous session to keep the seat.
void game_connected(game *g, string room, string name, string token,
                    u64 now_ms);
// The transport closed.
void game_disconnected(game *g, const char *reason);
// We left the room on purpose: back offline with no error, and the room's
// chat and drawing forgotten. The caller closes the connection.
void game_leave(game *g);

// One text frame from the server.
void game_on_message(game *g, const char *text, usize len, u64 now_ms);
// Heartbeats and stroke chunking; call every frame or at least every 33 ms.
void game_tick(game *g, u64 now_ms);

// Player actions. Each is ignored when it isn't allowed right now.
void game_start(game *g);
void game_choose(game *g, int index);
void game_guess(game *g, const char *text);
void game_set_brush(game *g, int color, int size);
void game_pen_down(game *g, float x, float y, float pressure, u64 now_ms);
void game_pen_move(game *g, float x, float y, float pressure, u64 now_ms);
void game_pen_up(game *g, u64 now_ms);
void game_undo(game *g);
void game_clear(game *g);

// Read-only views.
const game_state *game_get_state(const game *g);
const game_stroke *game_strokes(const game *g, usize *count);
// Chat lines oldest first; at most GAME_CHAT_LINES are kept.
const game_chat_line *game_chat(const game *g, usize *count);
const game_player *game_me(const game *g);
bool game_is_host(const game *g);
bool game_is_drawer(const game *g);
// True while we're the drawer and the drawing phase is on.
bool game_can_draw(const game *g);
int game_time_left_ms(const game *g, u64 now_ms);
// What changed since the last call (GAME_DIRTY_* bits); clears them.
u32 game_dirty(game *g);

// The 16-colour palette as 0xRRGGBB. Index 0 is white (the eraser).
extern const u32 game_palette[GAME_PALETTE_SIZE];

#endif

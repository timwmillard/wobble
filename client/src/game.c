#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "phx.h"

#define CLIENT_ID "wobble-raven/0.1"

// The smallest pen movement worth sending, in canvas units.
#define MIN_POINT_DIST 1.5f

// Removed strokes leave their points dead in the stroke arena until it is
// reset; the lobby canvas may never be, so copy the live strokes into the
// spare arena once this many points are dead (and outnumber the live ones).
#define COMPACT_DEAD_POINTS 65536

const u32 game_palette[GAME_PALETTE_SIZE] = {
    0xFFFFFF, 0x000000, 0x7F7F7F, 0xC1C1C1, 0xEF130B, 0xFF7100,
    0xFFE400, 0x00CC00, 0x00B2FF, 0x231FD3, 0xA300BA, 0xD37CAA,
    0xA0522D, 0x592F2A, 0xFFA5A5, 0x99EBFF,
};

typedef LIST(game_stroke) stroke_list;

struct game {
   game_send_fn send;
   void *user_data;
   game_state st;
   u32 dirty;

   // Where game_state's strings live:
   //   session  - you, token, and the channel below; reset on game_connected
   //   snap[]   - everything from the server's state snapshots, plus the word
   //              and choices for this turn. Each snapshot is built in the
   //              spare arena, then the two swap, so the old strings stay
   //              valid while the new ones are read out of the frame.
   //   errors   - st.error alone; reset each time it changes
   arena session;
   arena snap[2];
   int snap_cur; // the arena st's snapshot strings are in
   arena errors;

   arena scratch;         // one incoming frame at a time
   arena stroke_arena[2]; // strokes live in one; the other is for compacting
   int stroke_cur;
   usize dead_points;
   stroke_list strokes;

   game_chat_line chat[GAME_CHAT_LINES];
   usize chat_len;

   // The channel.
   string topic;
   string name;
   u64 next_ref;
   string join_ref; // of the current join
   u64 last_heartbeat_ms;

   // The pen.
   int brush_color, brush_size;
   bool pen_down;
   u32 pen_id; // our stroke in progress, while pen_down
   u32 next_stroke_id;
   usize sent; // points of the stroke in progress already sent
   u64 last_chunk_ms;
};

/* Small helpers */

// The first max bytes of s at most, cut at a UTF-8 character boundary.
static string utf8_prefix(string s, usize max) {
   if (s.len <= max)
      return s;
   usize n = max;
   while (n > 0 && ((u8)s.data[n] & 0xC0) == 0x80)
      n--;
   return string_slice(s, 0, (isize)n);
}

// Copies s into a fixed buffer, cut at a UTF-8 character boundary.
static void copy_str(char *dst, usize cap, string s) {
   s = utf8_prefix(s, cap - 1);
   if (s.len)
      memcpy(dst, s.data, s.len);
   dst[s.len] = '\0';
}

static arena *snap(game *g) {
   return &g->snap[g->snap_cur];
}

static int json_int(json *j, int fallback) {
   return (j && j->type == JSON_NUMBER) ? (int)j->number : fallback;
}

static bool json_bool(json *j) {
   return j && j->type == JSON_BOOL && j->boolean;
}

/* Chat */

static void add_chat(game *g, game_chat_kind kind, string from, string text) {
   if (g->chat_len == GAME_CHAT_LINES) {
      memmove(&g->chat[0], &g->chat[1],
              sizeof(g->chat[0]) * (GAME_CHAT_LINES - 1));
      g->chat_len--;
   }
   game_chat_line *line = &g->chat[g->chat_len++];
   line->kind = kind;
   copy_str(line->from, sizeof(line->from), from);
   copy_str(line->text, sizeof(line->text), text);
   g->dirty |= GAME_DIRTY_CHAT;
}

static game_chat_kind chat_kind(string kind) {
   if (string_eq(kind, S("system")))
      return GAME_CHAT_SYSTEM;
   if (string_eq(kind, S("correct")))
      return GAME_CHAT_CORRECT;
   if (string_eq(kind, S("secret")))
      return GAME_CHAT_SECRET;
   return GAME_CHAT_MSG;
}

/* Strokes */

static void reset_strokes(game *g) {
   arena_reset(&g->stroke_arena[0]);
   arena_reset(&g->stroke_arena[1]);
   g->strokes = (stroke_list)list_init(&g->stroke_arena[g->stroke_cur]);
   g->dead_points = 0;
   g->pen_down = false;
   g->dirty |= GAME_DIRTY_CANVAS;
}

static void compact_strokes(game *g) {
   arena *from = &g->stroke_arena[g->stroke_cur];
   arena *to = &g->stroke_arena[!g->stroke_cur];
   arena_reset(to);
   stroke_list out = list_init(to);
   list_reserve(&out, g->strokes.len);
   for (usize i = 0; i < g->strokes.len; i++) {
      game_stroke s = g->strokes.items[i];
      game_point_list pts = list_init(to);
      list_reserve(&pts, s.pts.len);
      if (s.pts.len)
         memcpy(pts.items, s.pts.items, s.pts.len * sizeof(*pts.items));
      pts.len = s.pts.len;
      s.pts = pts;
      s.by = string_dup(to, s.by);
      list_append(&out, s);
   }
   g->strokes = out;
   arena_reset(from);
   g->stroke_cur = !g->stroke_cur;
   g->dead_points = 0;
}

static bool stroke_is(const game_stroke *s, string by, u32 id) {
   return s->id == id && string_eq(s->by, by);
}

static game_stroke *find_open_stroke(game *g, string by, u32 id) {
   for (usize i = g->strokes.len; i-- > 0;) {
      game_stroke *s = &g->strokes.items[i];
      if (!s->done && stroke_is(s, by, id))
         return s;
   }
   return NULL;
}

// Our stroke in progress, or NULL when the pen is up or it was removed.
static game_stroke *pen_stroke(game *g) {
   return g->pen_down ? find_open_stroke(g, g->st.you, g->pen_id) : NULL;
}

static game_stroke *new_stroke(game *g, string by, u32 id, int color,
                               int size) {
   arena *a = &g->stroke_arena[g->stroke_cur];
   game_stroke s = {
       .by = string_dup(a, by),
       .id = id,
       .color = (u8)(color & 15),
       .size = (u8)(size < 1    ? 1
                    : size > 40 ? 40
                                : size),
   };
   s.pts = (game_point_list)list_init(a);
   list_append(&g->strokes, s);
   return &g->strokes.items[g->strokes.len - 1];
}

static i16 clamp_i16(int v, int lo, int hi) {
   return (i16)(v < lo ? lo : v > hi ? hi : v);
}

// One chunk from the server: {by, id, pts:[x,y,...], c, s, end, p?}. Each
// author's chunks join on to that author's open stroke of the same id, so
// several players can be mid-stroke at once.
static void apply_stroke(game *g, json *p) {
   json *pts = json_get(p, S("pts"));
   if (!pts || pts->type != JSON_ARRAY)
      return;
   string by = json_str(json_get(p, S("by")));
   u32 id = (u32)json_int(json_get(p, S("id")), 0);
   game_stroke *s = find_open_stroke(g, by, id);
   if (!s)
      s = new_stroke(g, by, id, json_int(json_get(p, S("c")), 1),
                     json_int(json_get(p, S("s")), 4));
   json *pressure = json_get(p, S("p"));
   for (usize i = 0; i + 1 < pts->len; i += 2) {
      int pv = json_int(json_at(pressure, i / 2), 255);
      game_point pt = {
          clamp_i16(json_int(json_at(pts, i), 0), 0, GAME_CANVAS_W),
          clamp_i16(json_int(json_at(pts, i + 1), 0), 0, GAME_CANVAS_H),
          (u8)(pv < 0     ? 0
               : pv > 255 ? 255
                          : pv),
      };
      list_append(&s->pts, pt);
   }
   s->done = json_bool(json_get(p, S("end")));
   g->dirty |= GAME_DIRTY_CANVAS;
}

// Removes by's strokes: the one with id, or all of them when all_ids.
static void remove_strokes(game *g, string by, u32 id, bool all_ids) {
   bool pen_gone =
       g->pen_down && string_eq(by, g->st.you) && (all_ids || id == g->pen_id);
   usize out = 0;
   for (usize i = 0; i < g->strokes.len; i++) {
      game_stroke *s = &g->strokes.items[i];
      if (string_eq(s->by, by) && (all_ids || s->id == id))
         g->dead_points += s->pts.len;
      else
         g->strokes.items[out++] = *s;
   }
   g->strokes.len = out;
   if (pen_gone)
      g->pen_down = false;
   if (g->dead_points >= COMPACT_DEAD_POINTS) {
      usize live = 0;
      for (usize i = 0; i < g->strokes.len; i++)
         live += g->strokes.items[i].pts.len;
      if (g->dead_points > live)
         compact_strokes(g);
   }
   g->dirty |= GAME_DIRTY_CANVAS;
}

/* Sending */

static string next_ref(game *g, arena *a) {
   return stringf(a, "%llu", (unsigned long long)++g->next_ref);
}

static void send_frame(game *g, string topic, string event, string payload,
                       bool with_join_ref) {
   if (!g->send)
      return;
   arena_temp t = arena_temp_begin(&g->scratch);
   string_builder sb = sb_init(&g->scratch);
   phx_write(&sb, with_join_ref ? g->join_ref : (string){0},
             next_ref(g, &g->scratch), topic, event, payload);
   g->send(sb.data, sb.len, g->user_data);
   arena_temp_end(t);
}

static bool joined(const game *g) {
   return g->st.phase >= GAME_LOBBY;
}

static void push(game *g, string event, string payload) {
   if (joined(g))
      send_frame(g, g->topic, event, payload, true);
}

static void send_join(game *g, u64 now_ms) {
   arena_temp t = arena_temp_begin(&g->scratch);
   string_builder p = sb_init(&g->scratch);
   sb_append(&p, S("{\"name\":"));
   json_write_string(&p, g->name);
   sb_append(&p, S(",\"client\":\"" CLIENT_ID "\""));
   if (g->st.token.len) {
      sb_append(&p, S(",\"token\":"));
      json_write_string(&p, g->st.token);
   }
   sb_push(&p, '}');

   // The join's ref doubles as the join_ref of everything on this channel.
   // A rejoin adds a few bytes to the session arena; there are few of them.
   g->join_ref =
       stringf(&g->session, "%llu", (unsigned long long)(g->next_ref + 1));
   send_frame(g, g->topic, S("phx_join"), sb_string(&p), true);
   arena_temp_end(t);

   g->st.phase = GAME_JOINING;
   g->last_heartbeat_ms = now_ms;
   g->dirty |= GAME_DIRTY_PHASE;
}

// The points of the stroke in progress not yet sent, plus the last sent one so
// each chunk joins on to the one before.
static void send_chunk(game *g, game_stroke *s, bool end, u64 now_ms) {
   usize from = g->sent > 0 ? g->sent - 1 : 0;
   if (s->pts.len == 0 || (from + 1 >= s->pts.len && g->sent && !end))
      return;

   arena_temp t = arena_temp_begin(&g->scratch);
   string_builder p = sb_init(&g->scratch);
   bool has_pressure = false;
   sb_appendf(&p, "{\"id\":%u,\"pts\":[", (unsigned)s->id);
   for (usize i = from; i < s->pts.len; i++) {
      sb_appendf(&p, "%s%d,%d", i > from ? "," : "", s->pts.items[i].x,
                 s->pts.items[i].y);
      has_pressure |= s->pts.items[i].pressure != 255;
   }
   sb_push(&p, ']');
   if (has_pressure) {
      sb_append(&p, S(",\"p\":["));
      for (usize i = from; i < s->pts.len; i++)
         sb_appendf(&p, "%s%u", i > from ? "," : "",
                    (unsigned)s->pts.items[i].pressure);
      sb_push(&p, ']');
   }
   sb_appendf(&p, ",\"c\":%u,\"s\":%u,\"end\":%s}", (unsigned)s->color,
              (unsigned)s->size, end ? "true" : "false");
   push(g, S("stroke"), sb_string(&p));
   arena_temp_end(t);

   g->sent = s->pts.len;
   g->last_chunk_ms = now_ms;
}

/* Incoming state */

static game_phase phase_from(string s) {
   if (string_eq(s, S("choosing")))
      return GAME_CHOOSING;
   if (string_eq(s, S("drawing")))
      return GAME_DRAWING;
   if (string_eq(s, S("reveal")))
      return GAME_REVEAL;
   if (string_eq(s, S("game_over")))
      return GAME_OVER;
   return GAME_LOBBY;
}

static void clear_choices(game *g) {
   g->st.has_choices = false;
   for (int i = 0; i < 3; i++)
      g->st.choices[i] = S("");
   g->dirty |= GAME_DIRTY_CHOICES;
}

static void apply_state(game *g, json *s, u64 now_ms) {
   game_state *st = &g->st;
   int turn = json_int(json_get(s, S("turn")), st->turn);
   game_phase phase = phase_from(json_str(json_get(s, S("phase"))));

   // Build the new snapshot's strings in the spare arena. The word and
   // choices carry over from the old one unless cleared below.
   arena *next = &g->snap[!g->snap_cur];
   arena_reset(next);
   st->word = string_dup(next, st->word);
   for (int i = 0; i < 3; i++)
      st->choices[i] = string_dup(next, st->choices[i]);

   // A new turn starts with a blank canvas and no word.
   if (turn != st->turn) {
      if (phase == GAME_CHOOSING || phase == GAME_LOBBY) {
         reset_strokes(g);
         st->word = S("");
         clear_choices(g);
      }
   }
   if (phase != GAME_CHOOSING && st->has_choices)
      clear_choices(g);

   st->phase = phase;
   st->turn = turn;
   st->round = json_int(json_get(s, S("round")), 0);
   st->rounds = json_int(json_get(s, S("rounds")), 0);
   st->host = string_dup(next, json_str(json_get(s, S("host"))));
   st->drawer = string_dup(next, json_str(json_get(s, S("drawer"))));
   st->hint = string_dup(next, json_str(json_get(s, S("hint"))));
   string word = json_str(json_get(s, S("word")));
   if (word.len)
      st->word = string_dup(next, word);

   int left = json_int(json_get(s, S("time_left_ms")), 0);
   st->deadline_ms = left > 0 ? now_ms + (u64)left : 0;

   json *players = json_get(s, S("players"));
   st->player_count = 0;
   for (usize i = 0; players && i < players->len; i++) {
      if (st->player_count == GAME_MAX_PLAYERS)
         break;
      json *p = json_at(players, i);
      game_player *gp = &st->players[st->player_count++];
      gp->id = string_dup(next, json_str(json_get(p, S("id"))));
      gp->name = string_dup(next, json_str(json_get(p, S("name"))));
      gp->score = json_int(json_get(p, S("score")), 0);
      gp->guessed = json_bool(json_get(p, S("guessed")));
      gp->drawing = json_bool(json_get(p, S("drawing")));
      gp->connected = json_bool(json_get(p, S("connected")));
   }
   g->snap_cur = !g->snap_cur;
   g->dirty |= GAME_DIRTY_PHASE | GAME_DIRTY_PLAYERS;
}

// text must not be the current error.
static void set_error(game *g, string text) {
   arena_reset(&g->errors);
   g->st.error = string_dup(&g->errors, text);
   g->dirty |= GAME_DIRTY_ERROR;
}

static void on_join_reply(game *g, const phx_msg *m, u64 now_ms) {
   json *r = phx_reply_response(m);
   if (!string_eq(phx_reply_status(m), S("ok"))) {
      string reason = json_str(json_get(r, S("reason")));
      set_error(g, reason.len ? reason : S("join failed"));
      g->st.phase = GAME_OFFLINE;
      g->dirty |= GAME_DIRTY_PHASE;
      return;
   }
   g->st.you = string_dup(&g->session, json_str(json_get(r, S("you"))));
   g->st.token = string_dup(&g->session, json_str(json_get(r, S("token"))));
   set_error(g, S(""));

   g->st.turn = -1; // so the state below counts as a new turn
   apply_state(g, json_get(r, S("state")), now_ms);
   reset_strokes(g);
   json *strokes = json_get(r, S("strokes"));
   for (usize i = 0; strokes && i < strokes->len; i++)
      apply_stroke(g, json_at(strokes, i));
   // Back in our seat after a restart: carry on past our own strokes' ids.
   for (usize i = 0; i < g->strokes.len; i++) {
      const game_stroke *s = &g->strokes.items[i];
      if (string_eq(s->by, g->st.you) && s->id > g->next_stroke_id)
         g->next_stroke_id = s->id;
   }
}

static void handle(game *g, const phx_msg *m, u64 now_ms) {
   json *p = m->payload;
   string ev = m->event;

   if (string_eq(ev, S("phx_reply"))) {
      if (string_eq(g->join_ref, m->ref) && g->st.phase == GAME_JOINING) {
         on_join_reply(g, m, now_ms);
      } else if (!string_eq(phx_reply_status(m), S("ok"))) {
         string reason = json_str(json_get(phx_reply_response(m), S("reason")));
         if (reason.len)
            set_error(g, reason);
      }
      return;
   }
   if (!string_eq(g->topic, m->topic))
      return;
   // Stale frames from an earlier join of this topic.
   if (m->join_ref.len && !string_eq(g->join_ref, m->join_ref))
      return;

   if (string_eq(ev, S("state"))) {
      apply_state(g, p, now_ms);
   } else if (string_eq(ev, S("choices"))) {
      json *words = json_get(p, S("words"));
      for (usize i = 0; i < 3; i++)
         g->st.choices[i] = string_dup(snap(g), json_str(json_at(words, i)));
      g->st.has_choices = words && words->len == 3;
      g->dirty |= GAME_DIRTY_CHOICES;
   } else if (string_eq(ev, S("word"))) {
      g->st.word = string_dup(snap(g), json_str(json_get(p, S("word"))));
      g->dirty |= GAME_DIRTY_PHASE;
   } else if (string_eq(ev, S("stroke"))) {
      apply_stroke(g, p);
   } else if (string_eq(ev, S("undo"))) {
      remove_strokes(g, json_str(json_get(p, S("by"))),
                     (u32)json_int(json_get(p, S("id")), -1), false);
   } else if (string_eq(ev, S("clear"))) {
      // With a "by", only that player's strokes (a guest clearing their own
      // doodles); otherwise the whole canvas.
      string by = json_str(json_get(p, S("by")));
      if (by.len)
         remove_strokes(g, by, 0, true);
      else
         reset_strokes(g);
   } else if (string_eq(ev, S("chat"))) {
      add_chat(g, chat_kind(json_str(json_get(p, S("kind")))),
               json_str(json_get(p, S("from"))),
               json_str(json_get(p, S("text"))));
   } else if (string_eq(ev, S("close"))) {
      arena_temp t = arena_temp_begin(&g->scratch);
      string text = stringf(&g->scratch, "'" STR_FMT "' is close!",
                            STR_ARG(json_str(json_get(p, S("text")))));
      add_chat(g, GAME_CHAT_CLOSE, S(""), text);
      arena_temp_end(t);
   } else if (string_eq(ev, S("phx_error")) || string_eq(ev, S("phx_close"))) {
      // The channel died on the server; join again and keep our seat.
      send_join(g, now_ms);
   }
}

/* Public API */

game *game_new(game_send_fn send, void *user_data) {
   game *g = calloc(1, sizeof(*g));
   if (!g)
      return NULL;
   g->send = send;
   g->user_data = user_data;
   g->strokes = (stroke_list)list_init(&g->stroke_arena[0]);
   game_state *st = &g->st;
   st->you = st->token = st->host = st->drawer = st->hint = st->word =
       st->error = S("");
   for (int i = 0; i < 3; i++)
      st->choices[i] = S("");
   g->topic = g->name = g->join_ref = S("");
   g->brush_color = 1;
   g->brush_size = 6;
   return g;
}

void game_free(game *g) {
   if (!g)
      return;
   arena_release(&g->session);
   arena_release(&g->snap[0]);
   arena_release(&g->snap[1]);
   arena_release(&g->errors);
   arena_release(&g->scratch);
   arena_release(&g->stroke_arena[0]);
   arena_release(&g->stroke_arena[1]);
   free(g);
}

void game_connected(game *g, string room, string name, string token,
                    u64 now_ms) {
   // The arguments may point into the session arena (our own token, say), so
   // hold copies while it is reset.
   arena_temp t = arena_temp_begin(&g->scratch);
   room = string_dup(&g->scratch, room);
   name = string_dup(&g->scratch, utf8_prefix(name, 64));
   token = string_dup(&g->scratch, token);
   arena_reset(&g->session);
   g->topic = stringf(&g->session, "room:" STR_FMT, STR_ARG(room));
   g->name = string_dup(&g->session, name);
   g->st.token = string_dup(&g->session, token);
   g->st.you = S("");
   arena_temp_end(t);
   send_join(g, now_ms);
}

void game_disconnected(game *g, const char *reason) {
   g->st.phase = GAME_OFFLINE;
   g->st.deadline_ms = 0;
   g->pen_down = false;
   set_error(g, string_view(reason && reason[0] ? reason : "disconnected"));
   g->dirty |= GAME_DIRTY_PHASE;
}

void game_leave(game *g) {
   g->st.phase = GAME_OFFLINE;
   g->st.deadline_ms = 0;
   g->st.player_count = 0;
   g->chat_len = 0;
   reset_strokes(g);
   set_error(g, S(""));
   g->dirty |= GAME_DIRTY_PHASE | GAME_DIRTY_PLAYERS | GAME_DIRTY_CHAT;
}

void game_on_message(game *g, const char *text, usize len, u64 now_ms) {
   phx_msg m;
   if (phx_parse(&g->scratch, string_view_bytes(text, len), &m))
      handle(g, &m, now_ms);
   arena_reset(&g->scratch);
}

void game_tick(game *g, u64 now_ms) {
   if (g->st.phase >= GAME_JOINING &&
       now_ms - g->last_heartbeat_ms >= GAME_HEARTBEAT_MS) {
      send_frame(g, S("phoenix"), S("heartbeat"), S("{}"), false);
      g->last_heartbeat_ms = now_ms;
   }
   game_stroke *s = pen_stroke(g);
   if (s && now_ms - g->last_chunk_ms >= GAME_CHUNK_MS)
      send_chunk(g, s, false, now_ms);
}

void game_start(game *g) {
   if (game_is_host(g) &&
       (g->st.phase == GAME_LOBBY || g->st.phase == GAME_OVER))
      push(g, S("start"), S("{}"));
}

void game_choose(game *g, int index) {
   if (index < 0 || index > 2 || !g->st.has_choices || !game_is_drawer(g))
      return;
   arena_temp t = arena_temp_begin(&g->scratch);
   push(g, S("choose"), stringf(&g->scratch, "{\"index\":%d}", index));
   arena_temp_end(t);
   g->st.has_choices = false;
   g->dirty |= GAME_DIRTY_CHOICES;
}

void game_guess(game *g, const char *text) {
   string t = string_trim(string_view(text));
   if (t.len == 0 || !joined(g))
      return;
   arena_temp tmp = arena_temp_begin(&g->scratch);
   string_builder p = sb_init(&g->scratch);
   sb_append(&p, S("{\"text\":"));
   json_write_string(&p, utf8_prefix(t, 400));
   sb_push(&p, '}');
   push(g, S("guess"), sb_string(&p));
   arena_temp_end(tmp);
}

void game_set_brush(game *g, int color, int size) {
   g->brush_color = color < 0 ? 0 : color >= GAME_PALETTE_SIZE ? 1 : color;
   g->brush_size = size < 1 ? 1 : size > 40 ? 40 : size;
}

static game_point make_point(float x, float y, float pressure) {
   game_point p = {
       clamp_i16((int)lroundf(x), 0, GAME_CANVAS_W),
       clamp_i16((int)lroundf(y), 0, GAME_CANVAS_H),
       255,
   };
   if (pressure >= 0.0f)
      p.pressure = (u8)lroundf((pressure > 1.0f ? 1.0f : pressure) * 255.0f);
   return p;
}

void game_pen_down(game *g, float x, float y, float pressure, u64 now_ms) {
   if (!game_can_draw(g))
      return;
   if (g->pen_down)
      game_pen_up(g, now_ms);
   g->pen_id = ++g->next_stroke_id;
   game_stroke *s =
       new_stroke(g, g->st.you, g->pen_id, g->brush_color, g->brush_size);
   list_append(&s->pts, make_point(x, y, pressure));
   g->pen_down = true;
   g->sent = 0;
   g->last_chunk_ms = now_ms;
   g->dirty |= GAME_DIRTY_CANVAS;
}

void game_pen_move(game *g, float x, float y, float pressure, u64 now_ms) {
   game_stroke *s = pen_stroke(g);
   if (!s || !game_can_draw(g))
      return;
   game_point last = s->pts.items[s->pts.len - 1];
   game_point pt = make_point(x, y, pressure);
   float dx = (float)(pt.x - last.x), dy = (float)(pt.y - last.y);
   if (dx * dx + dy * dy < MIN_POINT_DIST * MIN_POINT_DIST)
      return;
   list_append(&s->pts, pt);
   g->dirty |= GAME_DIRTY_CANVAS;
   if (s->pts.len - g->sent >= GAME_CHUNK_POINTS)
      send_chunk(g, s, false, now_ms);
}

void game_pen_up(game *g, u64 now_ms) {
   game_stroke *s = pen_stroke(g);
   g->pen_down = false;
   if (!s)
      return;
   s->done = true;
   if (game_can_draw(g))
      send_chunk(g, s, true, now_ms);
}

void game_undo(game *g) {
   if (!game_can_draw(g) || g->pen_down)
      return;
   for (usize i = g->strokes.len; i-- > 0;) {
      const game_stroke *s = &g->strokes.items[i];
      if (!string_eq(s->by, g->st.you))
         continue;
      u32 id = s->id;
      remove_strokes(g, g->st.you, id, false);
      arena_temp t = arena_temp_begin(&g->scratch);
      push(g, S("undo"), stringf(&g->scratch, "{\"id\":%u}", (unsigned)id));
      arena_temp_end(t);
      return;
   }
}

void game_clear(game *g) {
   if (!game_can_draw(g))
      return;
   if (game_is_practice(g) && !game_is_host(g))
      remove_strokes(g, g->st.you, 0, true);
   else
      reset_strokes(g);
   push(g, S("clear"), S("{}"));
}

const game_state *game_get_state(const game *g) {
   return &g->st;
}

const game_stroke *game_strokes(const game *g, usize *count) {
   *count = g->strokes.len;
   return g->strokes.items;
}

const game_chat_line *game_chat(const game *g, usize *count) {
   *count = g->chat_len;
   return g->chat;
}

const game_player *game_me(const game *g) {
   for (int i = 0; i < g->st.player_count; i++) {
      if (string_eq(g->st.players[i].id, g->st.you))
         return &g->st.players[i];
   }
   return NULL;
}

bool game_is_host(const game *g) {
   return g->st.you.len && string_eq(g->st.host, g->st.you);
}

bool game_is_drawer(const game *g) {
   return g->st.you.len && string_eq(g->st.drawer, g->st.you);
}

bool game_is_practice(const game *g) {
   return g->st.phase == GAME_LOBBY || g->st.phase == GAME_OVER;
}

bool game_can_draw(const game *g) {
   if (game_is_practice(g))
      return g->st.you.len > 0;
   return g->st.phase == GAME_DRAWING && game_is_drawer(g);
}

int game_time_left_ms(const game *g, u64 now_ms) {
   if (!g->st.deadline_ms || now_ms >= g->st.deadline_ms)
      return 0;
   return (int)(g->st.deadline_ms - now_ms);
}

u32 game_dirty(game *g) {
   u32 d = g->dirty;
   g->dirty = 0;
   return d;
}

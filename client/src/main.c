/* wobble — the native client: Raven UI over the game core.
 *
 *    wobble [--server ws://host:4000] [--room abcd] [--name Ana]
 *
 * With a room and a name it joins straight away. The network runs on webc's
 * threads; every transport callback is copied and handed to the UI thread
 * with Rtask_post, and only the UI thread touches the game.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Raven.h>

#include "canvas.h"
#include "game.h"
#include "gui_xml.h"
#include "transport.h"

static Rid window;
static Rid connect_screen, game_screen, name_input, room_input, server_input;
static Rid join_button, connect_error;
static Rid round_label, word_label, timer_label;
static Rid lobby_panel, lobby_text, start_button;
static Rid choose_panel, choose_title, choices_row, choice_buttons[3];
static Rid reveal_panel, reveal_word, over_panel, ranking, again_button;
static Rid tools, players_box, chat_box, chat_scroll, guess_input;

static game *g;
static transport *conn;
static u64 conn_gen; // events from an older connection are dropped
static char room[32], player_name[64];
// To keep our seat on a rejoin: the last token the server gave us, and the
// room it is for. They live in saved_arena, reset whenever they change.
static arena saved_arena;
static string saved_token, saved_room;

static u64 now_ms(void) {
   return (u64)(Rget_time() * 1000.0);
}

/* Small UI helpers */

static void set_text(Rid label, const char *text) {
   const char *old = Rlabel_get_text(label);
   if (!old || strcmp(old, text) != 0)
      Rlabel_set_text(label, text);
}

static void show(Rid widget, bool visible) {
   Rwidget_set_visibility(widget, visible ? R_VISIBLE : R_COLLAPSED);
}

static string player_name_of(string id) {
   const game_state *st = game_get_state(g);
   for (int i = 0; i < st->player_count; i++) {
      if (string_eq(st->players[i].id, id))
         return st->players[i].name;
   }
   return S("Someone");
}

/* Transport events, moved to the UI thread */

typedef enum { NET_OPEN, NET_MESSAGE, NET_CLOSE } net_kind;

typedef struct {
   u64 gen;
   net_kind kind;
   usize len;
   char data[];
} net_event;

static void refresh(u32 dirty);

static void on_net_event(void *user_data) {
   net_event *ev = user_data;
   if (ev->gen == conn_gen && g) {
      u64 now = now_ms();
      switch (ev->kind) {
      case NET_OPEN:
         game_connected(g, string_view(room), string_view(player_name),
                        string_eq(saved_room, string_view(room)) ? saved_token
                                                                 : S(""),
                        now);
         break;
      case NET_MESSAGE:
         game_on_message(g, ev->data, ev->len, now);
         break;
      case NET_CLOSE:
         game_disconnected(g, ev->data);
         break;
      }
      refresh(game_dirty(g));
   }
   free(ev);
}

static void post_net_event(void *user_data, net_kind kind, const char *data,
                           usize len) {
   net_event *ev = malloc(sizeof(*ev) + len + 1);
   if (!ev)
      return;
   ev->gen = (u64)(uintptr_t)user_data;
   ev->kind = kind;
   ev->len = len;
   memcpy(ev->data, data, len);
   ev->data[len] = '\0';
   Rtask_post(on_net_event, ev);
}

static void on_open(void *ud) {
   post_net_event(ud, NET_OPEN, "", 0);
}

static void on_message(const char *data, size_t len, void *ud) {
   post_net_event(ud, NET_MESSAGE, data, len);
}

static void on_close(const char *reason, void *ud) {
   post_net_event(ud, NET_CLOSE, reason, strlen(reason));
}

static void send_frame(const char *frame, usize len, void *user_data) {
   (void)user_data;
   transport_send(conn, frame, len);
}

/* Refreshing the UI from the game */

static void refresh_players(void) {
   const game_state *st = game_get_state(g);
   Rwidget_clear_children(players_box);
   for (int i = 0; i < st->player_count; i++) {
      const game_player *p = &st->players[i];
      Rid row = Rwidget_add_child(players_box, Rwidget_create(R_WIDGET_HBOX));
      Rpanel_set_spacing(row, 6.0f);
      Rslot_set_padding(row, 6.0f, 4.0f, 6.0f, 4.0f);

      char text[128];
      snprintf(text, sizeof(text), STR_FMT "%s%s", STR_ARG(p->name),
               string_eq(p->id, st->you) ? " (you)" : "",
               string_eq(p->id, st->host) ? " \xc2\xb7 host" : "");
      Rid name = Rwidget_add_child(row, Rlabel_create(text));
      Rslot_set_fill(name, 1.0f);
      if (p->guessed)
         Rlabel_set_color(name, 0.35f, 0.85f, 0.45f, 1.0f);
      if (p->drawing) {
         Rid tag = Rwidget_add_child(row, Rlabel_create("drawing"));
         Rlabel_set_color(tag, 0.45f, 0.65f, 1.0f, 1.0f);
         Rlabel_set_text_size(tag, 12.0f);
         Rslot_set_valign(tag, R_VALIGN_CENTER);
      }

      snprintf(text, sizeof(text), "%d", p->score);
      Rid score = Rwidget_add_child(row, Rlabel_create(text));
      Rlabel_set_font(score, Rfont_get_monospace());
      if (!p->connected)
         Rwidget_set_opacity(row, 0.4f);
   }
}

static void refresh_chat(void) {
   usize n;
   const game_chat_line *lines = game_chat(g, &n);
   Rwidget_clear_children(chat_box);
   // The last 60 lines are plenty on screen.
   for (usize i = n > 60 ? n - 60 : 0; i < n; i++) {
      const game_chat_line *l = &lines[i];
      char text[512];
      if (l->kind == GAME_CHAT_MSG || l->kind == GAME_CHAT_SECRET)
         snprintf(text, sizeof(text), "%s: %s", l->from, l->text);
      else
         snprintf(text, sizeof(text), "%s", l->text);
      Rid label = Rwidget_add_child(chat_box, Rlabel_create(text));
      Rlabel_set_wrap(label, R_TRUE);
      switch (l->kind) {
      case GAME_CHAT_SYSTEM:
         Rlabel_set_color(label, 0.55f, 0.65f, 0.85f, 1.0f);
         break;
      case GAME_CHAT_CORRECT:
         Rlabel_set_color(label, 0.35f, 0.85f, 0.45f, 1.0f);
         break;
      case GAME_CHAT_SECRET:
         Rlabel_set_color(label, 0.75f, 0.55f, 0.95f, 1.0f);
         break;
      case GAME_CHAT_CLOSE:
         Rlabel_set_color(label, 1.0f, 0.7f, 0.3f, 1.0f);
         break;
      case GAME_CHAT_MSG:
         break;
      }
   }
   Rscroll_box_scroll_to_end(chat_scroll);
}

// The hint with spaces between letters, so "____" reads as four blanks.
static void spaced(char *out, usize cap, string hint) {
   usize o = 0;
   for (usize i = 0; i < hint.len && o + 3 < cap; i++) {
      if (hint.data[i] == ' ') {
         out[o++] = ' ';
         out[o++] = ' ';
      } else {
         out[o++] = hint.data[i];
         out[o++] = ' ';
      }
   }
   out[o] = '\0';
}

static void refresh_ranking(void) {
   const game_state *st = game_get_state(g);
   int order[GAME_MAX_PLAYERS];
   for (int i = 0; i < st->player_count; i++)
      order[i] = i;
   for (int i = 1; i < st->player_count; i++) { // insertion sort by score
      for (int k = i; k > 0 && st->players[order[k]].score >
                                   st->players[order[k - 1]].score;
           k--) {
         int t = order[k];
         order[k] = order[k - 1];
         order[k - 1] = t;
      }
   }
   Rwidget_clear_children(ranking);
   for (int i = 0; i < st->player_count; i++) {
      const game_player *p = &st->players[order[i]];
      char text[128];
      snprintf(text, sizeof(text), "%d.  " STR_FMT "   %d", i + 1,
               STR_ARG(p->name), p->score);
      Rid label = Rwidget_add_child(ranking, Rlabel_create(text));
      if (i == 0)
         Rlabel_set_text_size(label, 20.0f);
   }
}

static void refresh_phase(void) {
   const game_state *st = game_get_state(g);
   bool in_room = st->phase >= GAME_LOBBY;
   show(connect_screen, !in_room);
   show(game_screen, in_room);
   Rwidget_set_enabled(join_button, st->phase == GAME_OFFLINE);
   Rbutton_set_text(join_button,
                    st->phase == GAME_OFFLINE ? "Join room" : "Joining...");
   if (!in_room)
      return;

   char text[256];
   if (st->phase == GAME_LOBBY)
      snprintf(text, sizeof(text), "Room %s", room);
   else
      snprintf(text, sizeof(text), "Round %d of %d", st->round, st->rounds);
   set_text(round_label, text);

   if (st->phase == GAME_DRAWING && st->word.len) {
      snprintf(text, sizeof(text), "%s" STR_FMT,
               game_is_drawer(g) ? "Draw: " : "You got it: ", STR_ARG(st->word));
      set_text(word_label, text);
   } else if (st->phase == GAME_DRAWING) {
      spaced(text, sizeof(text), st->hint);
      set_text(word_label, text);
   } else if (st->phase == GAME_CHOOSING) {
      snprintf(text, sizeof(text), STR_FMT " is choosing a word",
               STR_ARG(player_name_of(st->drawer)));
      set_text(word_label, game_is_drawer(g) ? "Pick a word to draw" : text);
   } else {
      set_text(word_label, "");
   }

   show(lobby_panel, st->phase == GAME_LOBBY);
   if (st->phase == GAME_LOBBY) {
      int connected = 0;
      for (int i = 0; i < st->player_count; i++)
         connected += st->players[i].connected;
      if (game_is_host(g))
         snprintf(text, sizeof(text), "%d player%s here. Start when ready.",
                  connected, connected == 1 ? "" : "s");
      else
         snprintf(text, sizeof(text), "%d player%s here. Waiting for " STR_FMT ".",
                  connected, connected == 1 ? "" : "s",
                  STR_ARG(player_name_of(st->host)));
      set_text(lobby_text, text);
      show(start_button, game_is_host(g));
      Rwidget_set_enabled(start_button, connected >= 2);
   }

   show(choose_panel, st->phase == GAME_CHOOSING);
   show(choices_row, st->phase == GAME_CHOOSING && st->has_choices);
   if (st->phase == GAME_CHOOSING) {
      if (game_is_drawer(g)) {
         set_text(choose_title, "Choose a word to draw");
      } else {
         snprintf(text, sizeof(text), STR_FMT " is choosing a word...",
                  STR_ARG(player_name_of(st->drawer)));
         set_text(choose_title, text);
      }
      for (int i = 0; i < 3; i++)
         Rbutton_set_text(choice_buttons[i], st->choices[i].data);
   }

   show(reveal_panel, st->phase == GAME_REVEAL);
   set_text(reveal_word, st->word.len ? st->word.data : "(nobody drew)");

   show(over_panel, st->phase == GAME_OVER);
   show(again_button, game_is_host(g));
   if (st->phase == GAME_OVER)
      refresh_ranking();

   show(tools, game_can_draw(g));
   Rtext_input_set_placeholder(guess_input, game_is_drawer(g) &&
                                                    st->phase == GAME_DRAWING
                                                ? "Chat (no spoilers!)"
                                                : "Type your guess...");
}

static void refresh(u32 dirty) {
   if (dirty & GAME_DIRTY_ERROR)
      set_text(connect_error, game_get_state(g)->error.data);
   if (dirty & (GAME_DIRTY_PHASE | GAME_DIRTY_PLAYERS | GAME_DIRTY_CHOICES))
      refresh_phase();
   if (dirty & GAME_DIRTY_PLAYERS)
      refresh_players();
   if (dirty & GAME_DIRTY_CHAT)
      refresh_chat();
   if (dirty & GAME_DIRTY_CANVAS)
      canvas_invalidate();

   // Keep the seat for a reconnect.
   const game_state *st = game_get_state(g);
   if (st->token.len && (!string_eq(st->token, saved_token) ||
                         !string_eq(string_view(room), saved_room))) {
      arena_reset(&saved_arena);
      saved_token = string_dup(&saved_arena, st->token);
      saved_room = string_from(&saved_arena, room);
   }
}

// WOBBLE_CAPTURE=out.ppm saves the window as a PPM image once, then quits;
// for screenshots in CI and docs. WOBBLE_CAPTURE_AFTER is seconds (default
// 2), or "choosing" / "drawing" to wait until we are the drawer then.
static void maybe_capture(u64 now) {
   static u64 at;
   const char *path = getenv("WOBBLE_CAPTURE");
   const char *after = getenv("WOBBLE_CAPTURE_AFTER");
   if (!path)
      return;
   if (after && strcmp(after, "choosing") == 0) {
      if (!game_get_state(g)->has_choices)
         return;
   } else if (after && strcmp(after, "drawing") == 0) {
      if (!game_can_draw(g)) {
         at = 0;
         return;
      }
      if (!at)
         at = now + 1500; // a moment for the tools to settle
      if (now < at)
         return;
   } else {
      if (!at) {
         at = now + (u64)((after ? atof(after) : 2.0) * 1000.0);
         return;
      }
      if (now < at)
         return;
   }
   int width = 0, height = 0;
   uint8_t *pixels = Rwindow_capture(window, &width, &height);
   FILE *file = pixels ? fopen(path, "wb") : NULL;
   if (file) {
      fprintf(file, "P6\n%d %d\n255\n", width, height);
      for (int i = 0; i < width * height; i++)
         fwrite(pixels + i * 4, 1, 3, file);
      fclose(file);
   }
   Rpixels_free(pixels);
   Rwindow_close(window);
}

static void on_tick(Rid timer, void *user_data) {
   (void)timer;
   (void)user_data;
   u64 now = now_ms();
   maybe_capture(now);
   game_tick(g, now);
   refresh(game_dirty(g));

   int left = game_time_left_ms(g, now);
   char text[32] = "";
   if (left > 0)
      snprintf(text, sizeof(text), "%d s", (left + 999) / 1000);
   set_text(timer_label, text);
}

/* Actions */

// Room codes are lower case letters, digits and dashes.
static bool clean_room(const char *in, char *out, usize cap) {
   usize n = 0;
   for (const char *p = in; *p; p++) {
      char ch = *p;
      if (ch >= 'A' && ch <= 'Z')
         ch = (char)(ch - 'A' + 'a');
      if (ch == ' ')
         continue;
      if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
         return false;
      if (n + 1 >= cap)
         return false;
      out[n++] = ch;
   }
   out[n] = '\0';
   return n > 0;
}

static void join(void) {
   const char *name = Rtext_input_get_text(name_input);
   const char *code = Rtext_input_get_text(room_input);
   const char *server = Rtext_input_get_text(server_input);
   if (!name || !string_trim(string_view(name)).len) {
      set_text(connect_error, "Enter your name.");
      Rwidget_focus(name_input);
      return;
   }
   if (!code || !clean_room(code, room, sizeof(room))) {
      set_text(connect_error, "Room codes use letters, digits and dashes.");
      Rwidget_focus(room_input);
      return;
   }
   snprintf(player_name, sizeof(player_name), "%s", name);
   set_text(connect_error, "");

   transport_free(conn);
   conn_gen++;
   conn = transport_connect(server ? server : "",
                            (transport_callbacks){
                                .on_open = on_open,
                                .on_message = on_message,
                                .on_close = on_close,
                                .user_data = (void *)(uintptr_t)conn_gen,
                            });
   if (!conn) {
      set_text(connect_error, "That server address doesn't look right.");
      return;
   }
   Rwidget_set_enabled(join_button, R_FALSE);
   Rbutton_set_text(join_button, "Connecting...");
}

static void on_join(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   if (game_get_state(g)->phase == GAME_OFFLINE)
      join();
}

static void on_start(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   game_start(g);
}

static void on_choose(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   game_choose(g, (int)(intptr_t)user_data);
   refresh(game_dirty(g));
}

static void on_guess(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   const char *text = Rtext_input_get_text(guess_input);
   if (text && text[0]) {
      game_guess(g, text);
      Rtext_input_set_text(guess_input, "");
   }
   Rwidget_focus(guess_input);
}

static void on_undo(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   game_undo(g);
   refresh(game_dirty(g));
}

static void on_clear(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   game_clear(g);
   refresh(game_dirty(g));
}

static Rid find(const char *name) {
   Rid w = Rwidget_find(window, name);
   if (w == R_INVALID_ID)
      fprintf(stderr, "gui.xml: no widget named %s\n", name);
   return w;
}

int main(int argc, char *argv[]) {
   const char *arg_server = NULL, *arg_room = NULL, *arg_name = NULL;
   for (int i = 1; i + 1 < argc; i += 2) {
      if (strcmp(argv[i], "--server") == 0)
         arg_server = argv[i + 1];
      else if (strcmp(argv[i], "--room") == 0)
         arg_room = argv[i + 1];
      else if (strcmp(argv[i], "--name") == 0)
         arg_name = argv[i + 1];
   }

   Rinit(R_BACKEND_DEFAULT);
   g = game_new(send_frame, NULL);

   Raction_bind("ui.join", on_join, NULL);
   Raction_bind("ui.start", on_start, NULL);
   Raction_bind("ui.choose0", on_choose, (void *)(intptr_t)0);
   Raction_bind("ui.choose1", on_choose, (void *)(intptr_t)1);
   Raction_bind("ui.choose2", on_choose, (void *)(intptr_t)2);
   Raction_bind("ui.guess", on_guess, NULL);
   Raction_bind("ui.undo", on_undo, NULL);
   Raction_bind("ui.clear", on_clear, NULL);

   window = Rwindow_create_from_xml(gui_xml_data, gui_xml_len);
   if (window == R_INVALID_ID) {
      Rshutdown();
      return 1;
   }

   connect_screen = find("connect_screen");
   game_screen = find("game_screen");
   name_input = find("name");
   room_input = find("room");
   server_input = find("server");
   join_button = find("join");
   connect_error = find("connect_error");
   round_label = find("round");
   word_label = find("word");
   timer_label = find("timer");
   lobby_panel = find("lobby_panel");
   lobby_text = find("lobby_text");
   start_button = find("start");
   choose_panel = find("choose_panel");
   choose_title = find("choose_title");
   choices_row = find("choices");
   for (int i = 0; i < 3; i++) {
      char name[16];
      snprintf(name, sizeof(name), "choice%d", i);
      choice_buttons[i] = find(name);
   }
   reveal_panel = find("reveal_panel");
   reveal_word = find("reveal_word");
   over_panel = find("over_panel");
   ranking = find("ranking");
   again_button = find("again");
   tools = find("tools");
   players_box = find("players");
   chat_box = find("chat");
   chat_scroll = find("chat_scroll");
   guess_input = find("guess");

   canvas_init(find("canvas"), find("palette"), g, now_ms);

   if (arg_server)
      Rtext_input_set_text(server_input, arg_server);
   if (arg_room)
      Rtext_input_set_text(room_input, arg_room);
   if (arg_name)
      Rtext_input_set_text(name_input, arg_name);
   Rwidget_focus(name_input);

   Rtimer_create(1.0 / 30.0, R_TRUE, on_tick, NULL);
   if (arg_room && arg_name)
      join();

   while (Rwait_frame(-1.0)) {
   }

   transport_free(conn);
   game_free(g);
   arena_release(&saved_arena);
   Rshutdown();
   return 0;
}

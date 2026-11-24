/* wobble — the client: Raven UI over the game core.
 *
 *    wobble [--server ws://host:4000] [--room abcd] [--name Ana] [link]
 *    wobble --practice
 *
 * The room field (and --room, or a bare argument) takes a code or a room
 * link, https://host/r/<code>, which names its own server. A bare code joins
 * the server field's server: --server, else $WOBBLE_SERVER, else the one the
 * build was configured with (WOBBLE_SERVER in CMakeLists.txt).
 *
 * In the browser web/shell.html passes the same arguments from the page:
 * the server is the page's own host, the room is /r/<code> (or ?room=), and
 * ?name= fills the name.
 *
 * --practice opens the drawing screen offline, to doodle alone.
 *
 * With a room and a name it joins straight away. Natively the network runs
 * on webc's threads; every transport callback is copied and handed to the UI
 * thread with Rtask_post, and only the UI thread touches the game.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <Raven.h>
#ifdef __EMSCRIPTEN__
#include <raven/raven_emscripten.h>
#endif

#include "canvas.h"
#include "game.h"
#include "art_svg.h"
#include "gui_xml.h"
#include "link.h"
#include "transport.h"
#include "words_txt.h"

#ifndef WOBBLE_SERVER
#define WOBBLE_SERVER "ws://localhost:4000"
#endif

static Rid window;
static Rid connect_screen, game_screen, name_input, room_input, server_input;
static Rid join_button, practice_button, connect_error, server_box;
static Rid server_toggle, new_word_button, side_panel, side_sep, leave_button;
static Rid copy_link_button, copy_invite_button, invite_label;
static Rid round_label, word_label, timer_label;
static Rid lobby_panel, lobby_text, start_button;
static Rid choose_panel, choose_title, choices_row, choice_buttons[3];
static Rid reveal_panel, reveal_word, over_panel, ranking, again_button;
static Rid tools, players_box, chat_box, chat_scroll, guess_input;

static game *g;
static transport *conn;
static u64 conn_gen; // events from an older connection are dropped
static char room[LINK_ROOM_MAX], player_name[64];
// The server we joined and the room's link to share, set on join.
static char server_url[LINK_SERVER_MAX], invite_url[LINK_SERVER_MAX + 64];
static u64 copied_until; // the copy buttons say "Copied!" until then
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

// Practice: just the canvas, the tools and a word to draw.
static void refresh_practice(void) {
   const game_state *st = game_get_state(g);
   char text[256] = "";
   if (st->word.len)
      snprintf(text, sizeof(text), "Draw: " STR_FMT, STR_ARG(st->word));
   set_text(round_label, "Practice");
   set_text(word_label, text);
   show(lobby_panel, false);
   show(choose_panel, false);
   show(reveal_panel, false);
   show(over_panel, false);
   show(tools, true);
}

static void refresh_phase(void) {
   const game_state *st = game_get_state(g);
   bool in_room = st->phase >= GAME_LOBBY;
   bool practising = st->phase == GAME_PRACTICE;
   static bool was_in_room;
   show(connect_screen, !in_room && !practising);
   show(game_screen, in_room || practising);
   // The focused field just went away with the connect screen; type to chat.
   if (in_room && !was_in_room)
      Rwidget_focus(guess_input);
   was_in_room = in_room;
   Rwidget_set_enabled(join_button, st->phase == GAME_OFFLINE);
   Rbutton_set_text(join_button,
                    st->phase == GAME_OFFLINE ? "Join room" : "Joining...");
   Rwidget_set_enabled(practice_button, st->phase == GAME_OFFLINE);
   // Room-only parts of the game screen.
   show(new_word_button, practising);
   show(copy_link_button, !practising);
   show(side_panel, !practising);
   show(side_sep, !practising);
   Rbutton_set_text(leave_button, practising ? "Back" : "Leave");
   if (practising)
      refresh_practice();
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
      set_text(invite_label, invite_url);
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

#ifdef __EMSCRIPTEN__
void wobble_show_room(const char *code); // web/wobble_lib.js
void wobble_show_home(void);
void wobble_copy_text(const char *text);
void wobble_open(const char *url);
static char shown_room[32]; // the room in the address bar, "" for none
#endif

// In the browser, once we are in a room the address bar shows its link,
// /r/<code>, ready to share.
static void show_room_in_url(void) {
#ifdef __EMSCRIPTEN__
   if (strcmp(shown_room, room) == 0)
      return;
   snprintf(shown_room, sizeof(shown_room), "%s", room);
   wobble_show_room(room);
#endif
}

// After leaving, so a reload doesn't drop us straight back in.
static void clear_room_from_url(void) {
#ifdef __EMSCRIPTEN__
   shown_room[0] = '\0';
   wobble_show_home();
#endif
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
   if (st->token.len) {
      if (!string_eq(st->token, saved_token) ||
          !string_eq(string_view(room), saved_room)) {
         arena_reset(&saved_arena);
         saved_token = string_dup(&saved_arena, st->token);
         saved_room = string_from(&saved_arena, room);
      }
      if (st->phase >= GAME_LOBBY)
         show_room_in_url();
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

   if (copied_until && now >= copied_until) {
      copied_until = 0;
      Rbutton_set_text(copy_link_button, "Copy link");
      Rbutton_set_text(copy_invite_button, "Copy invite link");
   }

   int left = game_time_left_ms(g, now);
   char text[32] = "";
   if (left > 0)
      snprintf(text, sizeof(text), "%d s", (left + 999) / 1000);
   set_text(timer_label, text);
}

/* Actions */

static void join(void) {
   const char *name = Rtext_input_get_text(name_input);
   const char *code = Rtext_input_get_text(room_input);
   const char *server = Rtext_input_get_text(server_input);

   link_target target;
   switch (link_parse(code, &target)) {
   case LINK_OK:
      break;
   case LINK_BAD_ROOM:
      set_text(connect_error, "Room codes use letters, digits and dashes.");
      Rwidget_focus(room_input);
      return;
   case LINK_NO_ROOM:
      set_text(connect_error, "That link doesn't name a room (.../r/abcd).");
      Rwidget_focus(room_input);
      return;
   case LINK_BAD_ADDRESS:
      set_text(connect_error, "That link doesn't look right.");
      Rwidget_focus(room_input);
      return;
   }
   // A link's ?name= helps only when we don't have one yet.
   if ((!name || !string_trim(string_view(name)).len) && target.name[0]) {
      Rtext_input_set_text(name_input, target.name);
      name = Rtext_input_get_text(name_input);
   }
   if (!name || !string_trim(string_view(name)).len) {
      set_text(connect_error, "Enter your name.");
      Rwidget_focus(name_input);
      return;
   }

   char own_server[LINK_SERVER_MAX];
   if (!link_server(server, own_server, sizeof(own_server))) {
      set_text(connect_error, "That server address doesn't look right.");
      show(server_box, true);
      Rwidget_focus(server_input);
      return;
   }
   const char *use = target.server[0] ? target.server : own_server;
#ifdef __EMSCRIPTEN__
   // A link to another host: that host serves its own page, so go there
   // (its socket wouldn't take us from this page's origin anyway).
   if (strcmp(use, own_server) != 0) {
      char url[sizeof(invite_url)];
      if (link_invite(use, target.room, url, sizeof(url)))
         wobble_open(url);
      return;
   }
#endif
   snprintf(room, sizeof(room), "%s", target.room);
   snprintf(server_url, sizeof(server_url), "%s", use);
   if (!link_invite(server_url, room, invite_url, sizeof(invite_url)))
      invite_url[0] = '\0';
   snprintf(player_name, sizeof(player_name), "%s", name);
   set_text(connect_error, "");

   transport_free(conn);
   conn_gen++;
   conn = transport_connect(server_url,
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

// Closing the socket is the whole goodbye: the server sees the channel go
// and tells the room we left. Our token is kept, so rejoining the same room
// soon after gets the seat (and score) back.
static void on_leave(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   transport_free(conn);
   conn = NULL;
   conn_gen++; // drop anything the old connection still has queued
   game_leave(g);
   refresh(game_dirty(g));
   clear_room_from_url();
   Rwidget_focus(room_input);
}

static void on_copy_link(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   if (!invite_url[0])
      return;
#ifdef __EMSCRIPTEN__
   wobble_copy_text(invite_url); // the page's clipboard isn't the system's
#else
   Rclipboard_set_text(invite_url);
#endif
   Rbutton_set_text(copy_link_button, "Copied!");
   Rbutton_set_text(copy_invite_button, "Copied!");
   copied_until = now_ms() + 2000;
}

// A random word from the server's list (embedded at build time), never the
// one showing now.
static string random_word(void) {
   static string words[1024];
   static usize count, last = (usize)-1;
   if (!count) {
      string rest = string_view(words_txt_data), line;
      while (rest.len && count < ARRAY_SIZE(words)) {
         string_cut(rest, S("\n"), &line, &rest);
         line = string_trim(line);
         if (line.len && line.data[0] != '#')
            words[count++] = line;
      }
      if (!count)
         return S("");
   }
   usize i = (usize)rand() % count;
   if (count > 1 && i == last)
      i = (i + 1 + (usize)rand() % (count - 1)) % count;
   last = i;
   return words[i];
}

// Practice needs no server: drop any connection still being made.
static void practice(void) {
   transport_free(conn);
   conn = NULL;
   conn_gen++;
   game_practice_start(g);
   game_practice_word(g, random_word());
   refresh(game_dirty(g));
   clear_room_from_url();
}

static void on_practice(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   if (game_get_state(g)->phase == GAME_OFFLINE)
      practice();
}

static void on_new_word(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   game_clear(g); // a new word gets a fresh page
   game_practice_word(g, random_word());
   refresh(game_dirty(g));
}

static void on_toggle_server(Rid sender, const char *action, void *user_data) {
   (void)sender;
   (void)action;
   (void)user_data;
   bool open = Rwidget_get_visibility(server_box) != R_VISIBLE;
   show(server_box, open);
   if (open)
      Rwidget_focus(server_input);
}

// A friend's link on the clipboard is most likely why the app was opened;
// put it in the room field. Only links: a copied word is not a room code.
static void room_from_clipboard(void) {
#ifndef __EMSCRIPTEN__
   if (!Rclipboard_has_text())
      return;
   const char *text = Rclipboard_get_text();
   link_target target;
   if (text && strlen(text) < 300 && link_parse(text, &target) == LINK_OK &&
       target.is_link)
      Rtext_input_set_text(room_input, text);
#endif
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

#ifdef __EMSCRIPTEN__
static void web_shutdown(void) {
   transport_free(conn);
   game_free(g);
   Rshutdown();
}
#endif

int main(int argc, char *argv[]) {
   const char *arg_server = NULL, *arg_room = NULL, *arg_name = NULL;
   bool arg_practice = false;
   for (int i = 1; i < argc; i++) {
      bool has_value = i + 1 < argc;
      if (strcmp(argv[i], "--practice") == 0)
         arg_practice = true;
      else if (strcmp(argv[i], "--server") == 0 && has_value)
         arg_server = argv[++i];
      else if (strcmp(argv[i], "--room") == 0 && has_value)
         arg_room = argv[++i];
      else if (strcmp(argv[i], "--name") == 0 && has_value)
         arg_name = argv[++i];
      else if (argv[i][0] != '-') // wobble https://host/r/abcd
         arg_room = argv[i];
   }

   Rinit(R_BACKEND_DEFAULT);
   Rtheme_set_metric(R_METRIC_FONT_SIZE,
                     Rtheme_get_metric(R_METRIC_FONT_SIZE) * 1.15f);
   g = game_new(send_frame, NULL);
   srand((unsigned)time(NULL)); // practice words

   // gui.xml names these by path; static data, so no copy.
   Rfile_register_memory("logo.svg", logo_svg_data, logo_svg_len, R_FALSE);
   Rfile_register_memory("doodles.svg", doodles_svg_data, doodles_svg_len,
                         R_FALSE);

   Raction_bind("ui.join", on_join, NULL);
   Raction_bind("ui.leave", on_leave, NULL);
   Raction_bind("ui.practice", on_practice, NULL);
   Raction_bind("ui.new_word", on_new_word, NULL);
   Raction_bind("ui.copy_link", on_copy_link, NULL);
   Raction_bind("ui.toggle_server", on_toggle_server, NULL);
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
   practice_button = find("practice");
   new_word_button = find("new_word");
   side_panel = find("side");
   side_sep = find("side_sep");
   leave_button = find("leave");
   connect_error = find("connect_error");
   server_box = find("server_box");
   server_toggle = find("server_toggle");
   copy_link_button = find("copy_link");
   copy_invite_button = find("copy_invite");
   invite_label = find("invite_link");
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

   const char *env_server = getenv("WOBBLE_SERVER");
   Rtext_input_set_text(server_input,
                        arg_server               ? arg_server
                        : env_server && *env_server ? env_server
                                                    : WOBBLE_SERVER);
#ifdef __EMSCRIPTEN__
   // The page's own host is the server; there is nothing to choose.
   show(server_toggle, false);
#endif
   if (arg_room)
      Rtext_input_set_text(room_input, arg_room);
   else
      room_from_clipboard();
   if (arg_name)
      Rtext_input_set_text(name_input, arg_name);
   Rwidget_focus(name_input);

   Rtimer_create(1.0 / 30.0, R_TRUE, on_tick, NULL);
   if (arg_practice)
      practice();
   else if (arg_room && arg_name)
      join();

#ifdef __EMSCRIPTEN__
   // Returns at once; the browser drives frames from here on.
   Rweb_start_ex(web_shutdown);
   return 0;
#else
   while (Rwait_frame(-1.0)) {
   }
#endif

   transport_free(conn);
   game_free(g);
   arena_release(&saved_arena);
   Rshutdown();
   return 0;
}

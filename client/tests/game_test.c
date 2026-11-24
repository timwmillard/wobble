/* game_test — the game core against a fake transport: recorded frames in,
 * the frames it sends checked. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "json.h"
#include "phx.h"

static int failures;

#define CHECK(cond)                                                            \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,      \
                 #cond);                                                       \
         failures++;                                                           \
      }                                                                        \
   } while (0)

/* The fake transport: keeps every frame sent. */

#define MAX_SENT 256
static char *sent[MAX_SENT];
static int sent_count;

static void fake_send(const char *frame, usize len, void *user_data) {
   (void)user_data;
   if (sent_count < MAX_SENT) {
      sent[sent_count] = malloc(len + 1);
      memcpy(sent[sent_count], frame, len);
      sent[sent_count][len] = '\0';
      sent_count++;
   }
}

static void clear_sent(void) {
   for (int i = 0; i < sent_count; i++)
      free(sent[i]);
   sent_count = 0;
}

static arena test_arena;

// The last frame sent, parsed.
static phx_msg last_sent(void) {
   phx_msg m = {0};
   CHECK(sent_count > 0);
   if (sent_count > 0)
      CHECK(phx_parse(&test_arena, string_view(sent[sent_count - 1]), &m));
   return m;
}

static int count_event(const char *event) {
   int n = 0;
   for (int i = 0; i < sent_count; i++) {
      phx_msg m;
      if (phx_parse(&test_arena, string_view(sent[i]), &m) &&
          string_eq(m.event, string_view(event)))
         n++;
   }
   return n;
}

static void feed(game *g, const char *frame, u64 now) {
   game_on_message(g, frame, strlen(frame), now);
}

#define LOBBY_STATE                                                            \
   "{\"phase\":\"lobby\",\"turn\":0,\"round\":0,\"rounds\":3,"                 \
   "\"players\":[{\"id\":\"me\",\"name\":\"Ana\",\"score\":0,"                 \
   "\"guessed\":false,\"drawing\":false,\"connected\":true},"                  \
   "{\"id\":\"bo\",\"name\":\"Bo\",\"score\":0,\"guessed\":false,"             \
   "\"drawing\":false,\"connected\":true}],"                                   \
   "\"host\":\"me\",\"drawer\":null,\"hint\":\"\",\"time_left_ms\":0,"         \
   "\"word\":null}"

static game *joined_game(void) {
   game *g = game_new(fake_send, NULL);
   clear_sent();
   game_connected(g, S("abcd"), S("Ana"), S(""), 1000);
   feed(g,
        "[\"1\",\"1\",\"room:abcd\",\"phx_reply\",{\"status\":\"ok\","
        "\"response\":{\"you\":\"me\",\"token\":\"tok\",\"state\":" LOBBY_STATE
        ",\"strokes\":[]}}]",
        1000);
   game_dirty(g);
   clear_sent();
   return g;
}

static void state_frame(game *g, const char *phase, int turn,
                        const char *drawer, const char *extra, u64 now) {
   char buf[1024];
   snprintf(buf, sizeof(buf),
            "[\"1\",null,\"room:abcd\",\"state\",{\"phase\":\"%s\","
            "\"turn\":%d,\"round\":1,\"rounds\":3,\"players\":[],"
            "\"host\":\"me\",\"drawer\":\"%s\",\"time_left_ms\":10000%s}]",
            phase, turn, drawer, extra);
   feed(g, buf, now);
}

static void test_join(void) {
   game *g = game_new(fake_send, NULL);
   clear_sent();
   game_connected(g, S("abcd"), S("Ana \"A\""), S(""), 1000);
   phx_msg m = last_sent();
   CHECK(string_eq(m.join_ref, S("1")));
   CHECK(string_eq(m.ref, S("1")));
   CHECK(string_eq(m.topic, S("room:abcd")));
   CHECK(string_eq(m.event, S("phx_join")));
   CHECK(string_eq(json_str(json_get(m.payload, S("name"))), S("Ana \"A\"")));
   CHECK(game_get_state(g)->phase == GAME_JOINING);

   feed(g,
        "[\"1\",\"1\",\"room:abcd\",\"phx_reply\",{\"status\":\"ok\","
        "\"response\":{\"you\":\"me\",\"token\":\"tok\",\"state\":" LOBBY_STATE
        ",\"strokes\":[{\"id\":3,\"pts\":[1,2,3,4],\"c\":4,\"s\":5,"
        "\"end\":true}]}}]",
        1000);
   const game_state *st = game_get_state(g);
   CHECK(st->phase == GAME_LOBBY);
   CHECK(string_eq(st->you, S("me")));
   CHECK(string_eq(st->token, S("tok")));
   CHECK(st->player_count == 2);
   CHECK(string_eq(st->players[1].name, S("Bo")));
   CHECK(game_is_host(g));
   usize n;
   const game_stroke *s = game_strokes(g, &n);
   CHECK(n == 1 && s[0].id == 3 && s[0].pts.len == 2 && s[0].color == 4);
   CHECK(game_dirty(g) & GAME_DIRTY_PLAYERS);
   CHECK(game_dirty(g) == 0);

   // Start is sent with the join's join_ref.
   clear_sent();
   game_start(g);
   m = last_sent();
   CHECK(string_eq(m.event, S("start")));
   CHECK(string_eq(m.join_ref, S("1")));
   game_free(g);
}

static void test_join_error(void) {
   game *g = game_new(fake_send, NULL);
   game_connected(g, S("abcd"), S("Ana"), S(""), 0);
   feed(g,
        "[\"1\",\"1\",\"room:abcd\",\"phx_reply\",{\"status\":\"error\","
        "\"response\":{\"reason\":\"room_full\"}}]",
        0);
   CHECK(game_get_state(g)->phase == GAME_OFFLINE);
   CHECK(string_eq(game_get_state(g)->error, S("room_full")));
   game_free(g);
}

static void test_drawer_turn(void) {
   game *g = joined_game();
   state_frame(g, "choosing", 1, "me", "", 2000);
   CHECK(game_is_drawer(g));
   CHECK(!game_can_draw(g));
   CHECK(game_time_left_ms(g, 2000) == 10000);
   CHECK(game_time_left_ms(g, 5000) == 7000);

   feed(g,
        "[\"1\",null,\"room:abcd\",\"choices\",{\"words\":[\"cat\",\"dog\","
        "\"sun\"],\"time_left_ms\":9000}]",
        2000);
   CHECK(game_get_state(g)->has_choices);
   CHECK(string_eq(game_get_state(g)->choices[1], S("dog")));
   game_choose(g, 1);
   phx_msg m = last_sent();
   CHECK(string_eq(m.event, S("choose")));
   CHECK(json_get(m.payload, S("index"))->number == 1);

   // Drawing isn't allowed until the server says so.
   game_pen_down(g, 10, 10, -1, 2000);
   usize n;
   game_strokes(g, &n);
   CHECK(n == 0);

   state_frame(g, "drawing", 1, "me", "", 2100);
   feed(g, "[\"1\",null,\"room:abcd\",\"word\",{\"word\":\"dog\"}]", 2100);
   CHECK(string_eq(game_get_state(g)->word, S("dog")));
   CHECK(game_can_draw(g));

   clear_sent();
   game_set_brush(g, 4, 12);
   game_pen_down(g, 10.2f, 20.7f, -1, 3000);
   game_pen_move(g, 10.5f, 20.9f, -1, 3001); // too close: dropped
   for (int i = 1; i <= 70; i++)
      game_pen_move(g, 10.0f + (float)i * 3, 21, -1, 3002);
   // 64 waiting points send a chunk without waiting for the clock.
   CHECK(count_event("stroke") == 1);
   game_tick(g, 3010);
   CHECK(count_event("stroke") == 1);
   game_tick(g, 3040);
   CHECK(count_event("stroke") == 2);
   game_pen_move(g, 500, 500, -1, 3050);
   game_pen_up(g, 3060);
   CHECK(count_event("stroke") == 3);

   m = last_sent();
   CHECK(string_eq(m.event, S("stroke")));
   json *pts = json_get(m.payload, S("pts"));
   // The last chunk starts with the previous chunk's last point.
   CHECK(pts->len == 4);
   CHECK(json_at(pts, 2)->number == 500);
   CHECK(json_get(m.payload, S("end"))->boolean);
   CHECK(json_get(m.payload, S("c"))->number == 4);
   CHECK(json_get(m.payload, S("s"))->number == 12);
   CHECK(json_get(m.payload, S("p")) == NULL);

   const game_stroke *s = game_strokes(g, &n);
   CHECK(n == 1 && s[0].done && s[0].pts.len == 72);
   CHECK(s[0].pts.items[0].x == 10 && s[0].pts.items[0].y == 21);

   // Undo removes our last stroke here and on the server.
   u32 id = s[0].id;
   game_undo(g);
   game_strokes(g, &n);
   CHECK(n == 0);
   m = last_sent();
   CHECK(string_eq(m.event, S("undo")));
   CHECK((u32)json_get(m.payload, S("id"))->number == id);

   // Pressure travels as p when the device has it.
   game_pen_down(g, 100, 100, 0.5f, 4000);
   game_pen_up(g, 4001);
   m = last_sent();
   CHECK(json_get(m.payload, S("p")) != NULL);
   CHECK(json_at(json_get(m.payload, S("p")), 0)->number == 128);

   // Coordinates are clamped to the canvas.
   game_pen_down(g, -50, 9000, -1, 5000);
   game_pen_up(g, 5001);
   m = last_sent();
   pts = json_get(m.payload, S("pts"));
   CHECK(json_at(pts, 0)->number == 0 && json_at(pts, 1)->number == 600);

   // A new turn wipes the canvas and the word.
   state_frame(g, "choosing", 2, "bo", "", 6000);
   game_strokes(g, &n);
   CHECK(n == 0);
   CHECK(game_get_state(g)->word.len == 0);
   CHECK(!game_can_draw(g));
   game_free(g);
}

static void test_guesser(void) {
   game *g = joined_game();
   state_frame(g, "drawing", 1, "bo", ",\"hint\":\"_ _ g\"", 2000);
   CHECK(!game_can_draw(g));
   CHECK(string_eq(game_get_state(g)->hint, S("_ _ g")));

   feed(g,
        "[\"1\",null,\"room:abcd\",\"stroke\",{\"id\":1,\"pts\":[0,0,5,5],"
        "\"c\":2,\"s\":3,\"end\":false}]",
        2000);
   feed(g,
        "[\"1\",null,\"room:abcd\",\"stroke\",{\"id\":1,\"pts\":[5,5,9,9],"
        "\"c\":2,\"s\":3,\"end\":true}]",
        2000);
   feed(g,
        "[\"1\",null,\"room:abcd\",\"stroke\",{\"id\":2,\"pts\":[7,7],"
        "\"c\":1,\"s\":3,\"end\":true,\"p\":[40]}]",
        2000);
   usize n;
   const game_stroke *s = game_strokes(g, &n);
   CHECK(n == 2);
   CHECK(s[0].pts.len == 4 && s[0].done);
   CHECK(s[1].pts.items[0].pressure == 40);

   feed(g, "[\"1\",null,\"room:abcd\",\"undo\",{\"id\":1}]", 2000);
   s = game_strokes(g, &n);
   CHECK(n == 1 && s[0].id == 2);
   feed(g, "[\"1\",null,\"room:abcd\",\"clear\",{}]", 2000);
   game_strokes(g, &n);
   CHECK(n == 0);

   // Guessing sends the trimmed text; JSON escapes survive.
   clear_sent();
   game_guess(g, "  a \"dog\"\n ");
   phx_msg m = last_sent();
   CHECK(string_eq(m.event, S("guess")));
   CHECK(string_eq(json_str(json_get(m.payload, S("text"))), S("a \"dog\"")));
   game_guess(g, "   ");
   CHECK(sent_count == 1);

   feed(g,
        "[\"1\",null,\"room:abcd\",\"chat\",{\"from\":\"Bo\",\"from_id\":"
        "\"bo\",\"text\":\"hi \\u00e9\",\"kind\":\"msg\"}]",
        2000);
   feed(g, "[\"1\",null,\"room:abcd\",\"close\",{\"text\":\"dogg\"}]", 2000);
   feed(g,
        "[\"1\",null,\"room:abcd\",\"chat\",{\"from\":null,\"text\":\"Ana "
        "guessed the word!\",\"kind\":\"correct\"}]",
        2000);
   usize lines;
   const game_chat_line *chat = game_chat(g, &lines);
   CHECK(lines == 3);
   CHECK(strcmp(chat[0].from, "Bo") == 0);
   CHECK(strcmp(chat[0].text, "hi \xc3\xa9") == 0);
   CHECK(chat[1].kind == GAME_CHAT_CLOSE);
   CHECK(strstr(chat[1].text, "dogg") != NULL);
   CHECK(chat[2].kind == GAME_CHAT_CORRECT && chat[2].from[0] == '\0');

   feed(g, "[\"1\",null,\"room:abcd\",\"word\",{\"word\":\"dog\"}]", 2000);
   CHECK(string_eq(game_get_state(g)->word, S("dog")));

   // Frames from an older join, or another topic, are ignored.
   feed(g, "[\"9\",null,\"room:abcd\",\"clear\",{}]", 2000);
   feed(g,
        "[\"1\",null,\"room:zzzz\",\"stroke\",{\"id\":5,\"pts\":[1,1],"
        "\"c\":1,\"s\":1,\"end\":true}]",
        2000);
   game_strokes(g, &n);
   CHECK(n == 0);
   game_free(g);
}

static void remote_stroke(game *g, const char *by, int id, const char *pts,
                          bool end) {
   char buf[512];
   snprintf(buf, sizeof(buf),
            "[\"1\",null,\"room:abcd\",\"stroke\",{\"by\":\"%s\",\"id\":%d,"
            "\"pts\":[%s],\"c\":2,\"s\":3,\"end\":%s}]",
            by, id, pts, end ? "true" : "false");
   feed(g, buf, 2000);
}

static usize strokes_by(game *g, const char *by) {
   usize n, count = 0;
   const game_stroke *s = game_strokes(g, &n);
   for (usize i = 0; i < n; i++)
      count += string_eq(s[i].by, string_view(by));
   return count;
}

static const game_stroke *stroke_of(game *g, const char *by, u32 id) {
   usize n;
   const game_stroke *s = game_strokes(g, &n);
   for (usize i = 0; i < n; i++) {
      if (s[i].id == id && string_eq(s[i].by, string_view(by)))
         return &s[i];
   }
   return NULL;
}

static void practice_state(game *g, const char *phase, int turn,
                           const char *host) {
   char buf[512];
   snprintf(buf, sizeof(buf),
            "[\"1\",null,\"room:abcd\",\"state\",{\"phase\":\"%s\","
            "\"turn\":%d,\"round\":0,\"rounds\":3,\"players\":[],"
            "\"host\":\"%s\",\"drawer\":null,\"time_left_ms\":0}]",
            phase, turn, host);
   feed(g, buf, 2000);
}

static void test_practice(void) {
   game *g = joined_game();
   CHECK(game_is_practice(g));
   CHECK(game_can_draw(g));
   usize n;

   // We, Bo and Cy all draw a stroke 1 at once; each author's chunks join
   // their own stroke.
   game_pen_down(g, 100, 100, -1, 2000);
   remote_stroke(g, "bo", 1, "0,0,5,5", false);
   game_pen_move(g, 120, 100, -1, 2001);
   remote_stroke(g, "cy", 1, "50,50,60,60", false);
   remote_stroke(g, "bo", 1, "5,5,9,9", true);
   game_pen_move(g, 140, 100, -1, 2002);
   remote_stroke(g, "cy", 1, "60,60,70,70", true);
   game_pen_up(g, 2003);

   game_strokes(g, &n);
   CHECK(n == 3);
   const game_stroke *mine = stroke_of(g, "me", 1);
   CHECK(mine && mine->done && mine->pts.len == 3);
   CHECK(mine && mine->pts.items[2].x == 140);
   CHECK(stroke_of(g, "bo", 1) && stroke_of(g, "bo", 1)->pts.len == 4);
   CHECK(stroke_of(g, "cy", 1) && stroke_of(g, "cy", 1)->done);
   phx_msg m = last_sent();
   CHECK(string_eq(m.event, S("stroke")));
   CHECK(json_get(m.payload, S("end"))->boolean);
   CHECK(json_get(m.payload, S("pts"))->len == 6);

   // Bo's undo of their stroke 1 leaves ours and Cy's.
   feed(g, "[\"1\",null,\"room:abcd\",\"undo\",{\"id\":1,\"by\":\"bo\"}]",
        2000);
   CHECK(strokes_by(g, "bo") == 0);
   CHECK(strokes_by(g, "me") == 1 && strokes_by(g, "cy") == 1);

   // Our undo takes our own latest stroke, though Cy's is newer.
   clear_sent();
   game_undo(g);
   CHECK(strokes_by(g, "me") == 0 && strokes_by(g, "cy") == 1);
   m = last_sent();
   CHECK(string_eq(m.event, S("undo")));
   CHECK(json_get(m.payload, S("id"))->number == 1);
   // With none of ours left, undo does nothing.
   clear_sent();
   game_undo(g);
   CHECK(sent_count == 0);

   // A clear by one guest takes only their strokes; one without "by" (the
   // host's) takes everything.
   remote_stroke(g, "bo", 2, "1,1,2,2", true);
   feed(g, "[\"1\",null,\"room:abcd\",\"clear\",{\"by\":\"cy\"}]", 2000);
   CHECK(strokes_by(g, "cy") == 0 && strokes_by(g, "bo") == 1);
   feed(g, "[\"1\",null,\"room:abcd\",\"clear\",{}]", 2000);
   game_strokes(g, &n);
   CHECK(n == 0);

   // The server dropping our stroke in progress (the ink cap) lifts the pen.
   game_pen_down(g, 10, 10, -1, 3000);
   u32 pen = stroke_of(g, "me", 2) ? 2 : 0;
   CHECK(pen == 2);
   feed(g, "[\"1\",null,\"room:abcd\",\"undo\",{\"id\":2,\"by\":\"me\"}]",
        3000);
   clear_sent();
   game_pen_move(g, 50, 50, -1, 3001);
   game_tick(g, 3100);
   game_pen_up(g, 3101);
   CHECK(sent_count == 0);
   game_strokes(g, &n);
   CHECK(n == 0);

   // As host, our clear wipes everyone's.
   remote_stroke(g, "bo", 3, "1,1,2,2", true);
   game_pen_down(g, 10, 10, -1, 3200);
   game_pen_up(g, 3201);
   clear_sent();
   game_clear(g);
   game_strokes(g, &n);
   CHECK(n == 0);
   CHECK(string_eq(last_sent().event, S("clear")));

   // The game starting wipes the canvas, and the drawer rules are back.
   remote_stroke(g, "bo", 4, "1,1,2,2", true);
   state_frame(g, "choosing", 1, "bo", "", 4000);
   game_strokes(g, &n);
   CHECK(n == 0);
   CHECK(!game_is_practice(g) && !game_can_draw(g));
   state_frame(g, "drawing", 1, "bo", "", 4100);
   CHECK(!game_can_draw(g));
   clear_sent();
   game_clear(g);
   game_undo(g);
   CHECK(sent_count == 0);

   // After the game anyone can doodle again. Bo is host now, so our clear
   // takes only our strokes.
   remote_stroke(g, "bo", 5, "1,1,2,2", true);
   practice_state(g, "game_over", 2, "bo");
   CHECK(game_can_draw(g));
   game_strokes(g, &n);
   CHECK(n == 1); // the last drawing stays
   game_pen_down(g, 10, 10, -1, 5000);
   game_pen_up(g, 5001);
   clear_sent();
   game_clear(g);
   CHECK(strokes_by(g, "me") == 0 && strokes_by(g, "bo") == 1);
   CHECK(string_eq(last_sent().event, S("clear")));
   game_free(g);
}

// Back in our seat after a restart, new strokes don't reuse our old ids.
static void test_practice_rejoin_ids(void) {
   game *g = game_new(fake_send, NULL);
   clear_sent();
   game_connected(g, S("abcd"), S("Ana"), S("tok"), 1000);
   feed(g,
        "[\"1\",\"1\",\"room:abcd\",\"phx_reply\",{\"status\":\"ok\","
        "\"response\":{\"you\":\"me\",\"token\":\"tok\",\"state\":" LOBBY_STATE
        ",\"strokes\":[{\"by\":\"me\",\"id\":7,\"pts\":[1,2],\"c\":1,"
        "\"s\":2,\"end\":true},{\"by\":\"bo\",\"id\":40,\"pts\":[1,2],"
        "\"c\":1,\"s\":2,\"end\":true}]}}]",
        1000);
   game_pen_down(g, 10, 10, -1, 2000);
   game_pen_up(g, 2001);
   CHECK(json_get(last_sent().payload, S("id"))->number == 8);
   game_free(g);
}

// A long doodling session: removed strokes are compacted away and the live
// ones survive the move intact.
static void test_practice_compaction(void) {
   game *g = joined_game();
   remote_stroke(g, "bo", 1, "11,12,13,14", true);
   char pts[16 * 200];
   usize len = 0;
   for (int i = 0; i < 200; i++)
      len += (usize)snprintf(pts + len, sizeof(pts) - len, "%s%d,%d",
                             i ? "," : "", i, i);
   char frame[sizeof(pts) + 256];
   for (int k = 0; k < 400; k++) {
      snprintf(frame, sizeof(frame),
               "[\"1\",null,\"room:abcd\",\"stroke\",{\"by\":\"cy\","
               "\"id\":%d,\"pts\":[%s],\"c\":2,\"s\":3,\"end\":true}]",
               k + 2, pts);
      feed(g, frame, 2000);
      snprintf(frame, sizeof(frame),
               "[\"1\",null,\"room:abcd\",\"undo\",{\"id\":%d,\"by\":"
               "\"cy\"}]",
               k + 2);
      feed(g, frame, 2000);
   }
   usize n;
   const game_stroke *s = game_strokes(g, &n);
   CHECK(n == 1);
   CHECK(n == 1 && string_eq(s[0].by, S("bo")) && s[0].id == 1);
   CHECK(n == 1 && s[0].pts.len == 2 && s[0].pts.items[1].y == 14);
   game_free(g);
}

static void test_chat_overflow(void) {
   game *g = joined_game();
   char buf[256];
   for (int i = 0; i < GAME_CHAT_LINES + 10; i++) {
      snprintf(buf, sizeof(buf),
               "[\"1\",null,\"room:abcd\",\"chat\",{\"from\":\"Bo\","
               "\"text\":\"m%d\",\"kind\":\"msg\"}]",
               i);
      feed(g, buf, 0);
   }
   usize n;
   const game_chat_line *chat = game_chat(g, &n);
   CHECK(n == GAME_CHAT_LINES);
   CHECK(strcmp(chat[0].text, "m10") == 0);
   game_free(g);
}

static void test_leave(void) {
   game *g = joined_game();
   feed(g,
        "[\"1\",null,\"room:abcd\",\"chat\",{\"from\":\"Bo\","
        "\"text\":\"hi\",\"kind\":\"msg\"}]",
        0);
   game_dirty(g);
   game_leave(g);
   const game_state *st = game_get_state(g);
   CHECK(st->phase == GAME_OFFLINE);
   CHECK(st->player_count == 0);
   CHECK(st->error.len == 0);
   usize n;
   game_chat(g, &n);
   CHECK(n == 0);
   CHECK(game_dirty(g) & GAME_DIRTY_PHASE);
   // The token stays, so coming back to the room keeps the seat.
   CHECK(string_eq(st->token, S("tok")));
   game_free(g);
}

static void test_heartbeat(void) {
   game *g = joined_game();
   game_tick(g, 1000 + GAME_HEARTBEAT_MS - 1);
   CHECK(sent_count == 0);
   game_tick(g, 1000 + GAME_HEARTBEAT_MS);
   phx_msg m = last_sent();
   CHECK(string_eq(m.topic, S("phoenix")));
   CHECK(string_eq(m.event, S("heartbeat")));
   CHECK(m.join_ref.len == 0);
   game_free(g);
}

static void test_rejoin_after_channel_error(void) {
   game *g = joined_game();
   feed(g, "[\"1\",\"1\",\"room:abcd\",\"phx_error\",{}]", 5000);
   phx_msg m = last_sent();
   CHECK(string_eq(m.event, S("phx_join")));
   CHECK(string_eq(json_str(json_get(m.payload, S("token"))), S("tok")));
   CHECK(!string_eq(m.join_ref, S("1")));
   game_free(g);
}

// game_state's strings live in arenas the game swaps and resets; check that
// each survives as long as game.h says.
static void test_string_lifetimes(void) {
   game *g = joined_game();
   state_frame(g, "choosing", 1, "me", "", 1000);
   feed(g,
        "[\"1\",null,\"room:abcd\",\"choices\",{\"words\":[\"cat\",\"dog\","
        "\"emu\"],\"time_left_ms\":9000}]",
        1000);
   // Snapshots in the same phase keep the choices across arena swaps.
   for (int i = 0; i < 5; i++)
      state_frame(g, "choosing", 1, "me", "", 1100 + (u64)i);
   CHECK(game_get_state(g)->has_choices);
   CHECK(string_eq(game_get_state(g)->choices[2], S("emu")));

   // The word arrives once, then outlives many snapshots in its turn...
   state_frame(g, "drawing", 1, "me", "", 2000);
   feed(g, "[\"1\",null,\"room:abcd\",\"word\",{\"word\":\"dog\"}]", 2000);
   for (int i = 0; i < 5; i++)
      state_frame(g, "drawing", 1, "me", "", 2100 + (u64)i);
   CHECK(string_eq(game_get_state(g)->word, S("dog")));
   CHECK(string_eq(game_get_state(g)->drawer, S("me")));
   CHECK(game_is_drawer(g));
   // ...and goes when the next turn starts.
   state_frame(g, "choosing", 2, "bo", "", 3000);
   CHECK(game_get_state(g)->word.len == 0);
   CHECK(game_get_state(g)->word.data != NULL);
   CHECK(!game_get_state(g)->has_choices);

   // Reconnecting with the game's own token, which lives in the arena that
   // game_connected resets.
   game_disconnected(g, "closed");
   CHECK(string_eq(game_get_state(g)->error, S("closed")));
   clear_sent();
   game_connected(g, S("abcd"), S("Ana"), game_get_state(g)->token, 4000);
   phx_msg m = last_sent();
   CHECK(string_eq(m.event, S("phx_join")));
   CHECK(string_eq(json_str(json_get(m.payload, S("token"))), S("tok")));
   CHECK(string_eq(game_get_state(g)->token, S("tok")));
   game_free(g);
}

int main(void) {
   test_join();
   test_join_error();
   test_drawer_turn();
   test_guesser();
   test_practice();
   test_practice_rejoin_ids();
   test_practice_compaction();
   test_chat_overflow();
   test_leave();
   test_heartbeat();
   test_rejoin_after_channel_error();
   test_string_lifetimes();
   clear_sent();
   arena_release(&test_arena);
   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("game_test: ok\n");
   return 0;
}

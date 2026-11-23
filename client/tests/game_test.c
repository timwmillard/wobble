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
   game_connected(g, "abcd", "Ana", NULL, 1000);
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
   game_connected(g, "abcd", "Ana \"A\"", NULL, 1000);
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
   CHECK(strcmp(st->you, "me") == 0);
   CHECK(strcmp(st->token, "tok") == 0);
   CHECK(st->player_count == 2);
   CHECK(strcmp(st->players[1].name, "Bo") == 0);
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
   game_connected(g, "abcd", "Ana", NULL, 0);
   feed(g,
        "[\"1\",\"1\",\"room:abcd\",\"phx_reply\",{\"status\":\"error\","
        "\"response\":{\"reason\":\"room_full\"}}]",
        0);
   CHECK(game_get_state(g)->phase == GAME_OFFLINE);
   CHECK(strcmp(game_get_state(g)->error, "room_full") == 0);
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
   CHECK(strcmp(game_get_state(g)->choices[1], "dog") == 0);
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
   CHECK(strcmp(game_get_state(g)->word, "dog") == 0);
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
   CHECK(game_get_state(g)->word[0] == '\0');
   CHECK(!game_can_draw(g));
   game_free(g);
}

static void test_guesser(void) {
   game *g = joined_game();
   state_frame(g, "drawing", 1, "bo", ",\"hint\":\"_ _ g\"", 2000);
   CHECK(!game_can_draw(g));
   CHECK(strcmp(game_get_state(g)->hint, "_ _ g") == 0);

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
   CHECK(strcmp(game_get_state(g)->word, "dog") == 0);

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

int main(void) {
   test_join();
   test_join_error();
   test_drawer_turn();
   test_guesser();
   test_chat_overflow();
   test_heartbeat();
   test_rejoin_after_channel_error();
   clear_sent();
   arena_release(&test_arena);
   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("game_test: ok\n");
   return 0;
}

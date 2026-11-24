/* link_test — what the room field accepts, and the links we share. */

#include <stdio.h>
#include <string.h>

#include "link.h"

static int failures;

#define CHECK(cond)                                                            \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,      \
                 #cond);                                                       \
         failures++;                                                           \
      }                                                                        \
   } while (0)

static void check_parse(const char *text, link_result want_result,
                        const char *want_server, const char *want_room) {
   link_target t;
   link_result r = link_parse(text, &t);
   if (r != want_result || strcmp(t.server, want_server) != 0 ||
       (r == LINK_OK && strcmp(t.room, want_room) != 0)) {
      fprintf(stderr, "link_parse(\"%s\"): got %d \"%s\" \"%s\"\n", text, r,
              t.server, t.room);
      failures++;
   }
}

static void test_parse(void) {
   // Bare codes use the default server.
   check_parse("abcd", LINK_OK, "", "abcd");
   check_parse("  AB CD \n", LINK_OK, "", "abcd");
   check_parse("my-room-2", LINK_OK, "", "my-room-2");
   check_parse("ab!cd", LINK_BAD_ROOM, "", "");
   check_parse("", LINK_BAD_ROOM, "", "");

   // Full links.
   check_parse("https://wobblegame.fly.dev/r/abcd", LINK_OK,
               "wss://wobblegame.fly.dev", "abcd");
   check_parse("https://WobbleGame.fly.dev/r/ABCD/", LINK_OK,
               "wss://wobblegame.fly.dev", "abcd");
   check_parse("http://localhost:4000/r/abcd", LINK_OK, "ws://localhost:4000",
               "abcd");
   check_parse("wss://example.com/r/x1", LINK_OK, "wss://example.com", "x1");
   check_parse("ws://10.0.0.5:4000/r/x1", LINK_OK, "ws://10.0.0.5:4000",
               "x1");
   check_parse("wobble://wobblegame.fly.dev/r/abcd", LINK_OK,
               "wss://wobblegame.fly.dev", "abcd");

   // No scheme: guess from the host.
   check_parse("wobblegame.fly.dev/r/abcd", LINK_OK,
               "wss://wobblegame.fly.dev", "abcd");
   check_parse("localhost:4000/r/abcd", LINK_OK, "ws://localhost:4000",
               "abcd");
   check_parse("192.168.1.20:4000/r/abcd", LINK_OK,
               "ws://192.168.1.20:4000", "abcd");
   check_parse("mac.local:4000/r/abcd", LINK_OK, "ws://mac.local:4000",
               "abcd");

   // Query forms, and what isn't a room.
   check_parse("https://wobblegame.fly.dev/?room=abcd", LINK_OK,
               "wss://wobblegame.fly.dev", "abcd");
   check_parse("https://wobblegame.fly.dev/r/abcd?name=Ana#x", LINK_OK,
               "wss://wobblegame.fly.dev", "abcd");
   check_parse("https://wobblegame.fly.dev/", LINK_NO_ROOM,
               "wss://wobblegame.fly.dev", "");
   check_parse("https://wobblegame.fly.dev/r/a$b", LINK_BAD_ROOM,
               "wss://wobblegame.fly.dev", "");
   check_parse("https:///r/abcd", LINK_BAD_ADDRESS, "", "");
   check_parse("https://bad host/r/abcd", LINK_BAD_ADDRESS, "", "");

   link_target t;
   CHECK(link_parse("http://localhost:4000/r/abcd?name=Ana+B%C3%A9", &t) ==
         LINK_OK);
   CHECK(strcmp(t.name, "Ana B\xc3\xa9") == 0);
   CHECK(t.is_link);
   CHECK(link_parse("abcd", &t) == LINK_OK && !t.is_link);
}

static void test_server(void) {
   char out[LINK_SERVER_MAX];
   CHECK(link_server("ws://localhost:4000", out, sizeof(out)) &&
         strcmp(out, "ws://localhost:4000") == 0);
   CHECK(link_server(" localhost:4000 ", out, sizeof(out)) &&
         strcmp(out, "ws://localhost:4000") == 0);
   CHECK(link_server("https://wobblegame.fly.dev/", out, sizeof(out)) &&
         strcmp(out, "wss://wobblegame.fly.dev") == 0);
   CHECK(link_server("wobblegame.fly.dev", out, sizeof(out)) &&
         strcmp(out, "wss://wobblegame.fly.dev") == 0);
   CHECK(!link_server("", out, sizeof(out)));
   CHECK(!link_server("not a host", out, sizeof(out)));
}

static void test_invite(void) {
   char out[256];
   CHECK(link_invite("wss://wobblegame.fly.dev", "abcd", out, sizeof(out)) &&
         strcmp(out, "https://wobblegame.fly.dev/r/abcd") == 0);
   CHECK(link_invite("ws://localhost:4000", "abcd", out, sizeof(out)) &&
         strcmp(out, "http://localhost:4000/r/abcd") == 0);
   CHECK(!link_invite("localhost:4000", "abcd", out, sizeof(out)));
   CHECK(!link_invite("wss://wobblegame.fly.dev", "abcd", out, 10));

   // What we share reads back as the same server and room.
   link_target t;
   CHECK(link_invite("wss://example.com:8443", "x-1", out, sizeof(out)));
   CHECK(link_parse(out, &t) == LINK_OK);
   CHECK(strcmp(t.server, "wss://example.com:8443") == 0);
   CHECK(strcmp(t.room, "x-1") == 0);
}

int main(void) {
   test_parse();
   test_server();
   test_invite();
   if (failures) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   printf("link_test: ok\n");
   return 0;
}

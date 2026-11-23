/* wobble_bot — headless players on the same core and transport as the app.
 *
 *    wobble_bot [-v] [-s ws://localhost:4000] [-r room] [-n 8] [-p 9] [-t 600]
 *
 * -p waits for that many players (bots and people) before the host bot
 * starts, so people can play along with the bots.
 *
 * Starts n bots in one room. Whichever is host starts the game once
 * everyone is in. Drawers pick a word and scribble; guessers chat and
 * sometimes guess right (bots in one process share the drawer's word, which
 * real clients never see). Exits 0 when the game is over, after printing the
 * scores; 1 on a disconnect or a protocol check failing; 2 on timeout.
 *
 * Checks while playing: no guesser learns the word before guessing it.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include "transport.h"

#define MAX_BOTS GAME_MAX_PLAYERS

typedef enum { EV_OPEN, EV_MESSAGE, EV_CLOSE } event_kind;

typedef struct event event;
struct event {
   event *next;
   int bot;
   event_kind kind;
   size_t len;
   char data[];
};

// Transport callbacks run on network threads; they queue events here and the
// main loop applies them to the games.
static struct {
   pthread_mutex_t lock;
   event *head, *tail;
} queue = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL};

typedef struct {
   int index;
   char name[16];
   game *g;
   transport *t;
   u64 choose_at;      // when to pick a word, 0 = not yet seen choices
   u64 next_guess_at;  // when to chat next
   u64 next_stroke_at; // when to start or extend the next stroke
   int stroke_points;  // points left in the stroke in progress
   float x, y;         // the pen
   bool guessed_right; // we sent the right word this turn
   int turn;           // the turn the fields above are for
} bot;

static bot bots[MAX_BOTS];
static int bot_count = 8;
static int wait_for; // players in the room before starting; 0 = bot_count
static const char *room = "bots";
static bool started;
static bool verbose; // -v: print bot 1's frames
// The drawer's word, a bot-only cheat, and the turn it is for. Until the
// drawer bot has run in a new turn it still holds the last turn's word.
static char shared_word[128];
static int shared_turn = -1;

static const char *wrong_words[] = {"house", "tree", "banana", "cloud",
                                    "robot", "ship", "pizza", "cat"};

static u64 now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}

static float frand(float lo, float hi) {
   return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static void post(int index, event_kind kind, const char *data, size_t len) {
   event *ev = malloc(sizeof(*ev) + len + 1);
   if (!ev)
      return;
   ev->next = NULL;
   ev->bot = index;
   ev->kind = kind;
   ev->len = len;
   if (len)
      memcpy(ev->data, data, len);
   ev->data[len] = '\0';
   pthread_mutex_lock(&queue.lock);
   if (queue.tail)
      queue.tail->next = ev;
   else
      queue.head = ev;
   queue.tail = ev;
   pthread_mutex_unlock(&queue.lock);
}

static void on_open(void *ud) {
   post((int)(intptr_t)ud, EV_OPEN, "", 0);
}

static void on_message(const char *data, size_t len, void *ud) {
   post((int)(intptr_t)ud, EV_MESSAGE, data, len);
}

static void on_close(const char *reason, void *ud) {
   post((int)(intptr_t)ud, EV_CLOSE, reason, strlen(reason));
}

static void send_frame(const char *frame, usize len, void *ud) {
   bot *b = ud;
   if (verbose && b->index == 0)
      printf("> %.*s\n", (int)len, frame);
   transport_send(b->t, frame, len);
}

static event *take_events(void) {
   pthread_mutex_lock(&queue.lock);
   event *ev = queue.head;
   queue.head = queue.tail = NULL;
   pthread_mutex_unlock(&queue.lock);
   return ev;
}

static int connected_players(const game_state *st) {
   int n = 0;
   for (int i = 0; i < st->player_count; i++)
      n += st->players[i].connected;
   return n;
}

static void draw(bot *b, u64 now) {
   if (now < b->next_stroke_at)
      return;
   if (b->stroke_points == 0) {
      b->x = frand(50, GAME_CANVAS_W - 50);
      b->y = frand(50, GAME_CANVAS_H - 50);
      game_set_brush(b->g, 1 + rand() % 15, 2 + rand() % 12);
      game_pen_down(b->g, b->x, b->y, -1, now);
      b->stroke_points = 10 + rand() % 40;
   } else {
      b->x += frand(-15, 15);
      b->y += frand(-15, 15);
      game_pen_move(b->g, b->x, b->y, -1, now);
      if (--b->stroke_points == 0) {
         game_pen_up(b->g, now);
         b->next_stroke_at = now + 300;
         return;
      }
   }
   b->next_stroke_at = now + 10;
}

static void guess(bot *b, u64 now) {
   const game_player *me = game_me(b->g);
   if (!me || me->guessed || now < b->next_guess_at ||
       shared_turn != game_get_state(b->g)->turn)
      return;
   const char *wrong = wrong_words[rand() % 8];
   if (rand() % 4 == 0) {
      game_guess(b->g, shared_word);
      b->guessed_right = true;
   } else if (strcmp(wrong, shared_word) != 0) {
      game_guess(b->g, wrong);
   }
   // Well under the server's chat limit of 5 per 3 s.
   b->next_guess_at = now + 700 + (u64)(rand() % 1500);
}

// One step of a bot's play. False when a check failed.
static bool play(bot *b, u64 now) {
   const game_state *st = game_get_state(b->g);

   if (st->turn != b->turn) {
      b->turn = st->turn;
      b->choose_at = 0;
      b->guessed_right = false;
      b->stroke_points = 0;
   }

   if (game_is_host(b->g) && st->phase == GAME_LOBBY && !started &&
       connected_players(st) >= (wait_for ? wait_for : bot_count)) {
      game_start(b->g);
      started = true;
   }

   if (st->phase == GAME_CHOOSING && st->has_choices) {
      if (!b->choose_at)
         b->choose_at = now + 200 + (u64)(rand() % 400);
      else if (now >= b->choose_at)
         game_choose(b->g, rand() % 3);
   }

   if (st->phase != GAME_DRAWING)
      return true;

   if (game_is_drawer(b->g)) {
      snprintf(shared_word, sizeof(shared_word), STR_FMT, STR_ARG(st->word));
      shared_turn = st->word.len ? st->turn : -1;
      draw(b, now);
      return true;
   }

   // A guesser who hasn't guessed must not know the word.
   if (st->word.len && !b->guessed_right) {
      fprintf(stderr, "%s: knows the word '" STR_FMT "' without guessing it\n",
              b->name, STR_ARG(st->word));
      return false;
   }
   guess(b, now);
   return true;
}

static void print_scores(const game_state *st) {
   printf("game over after %d rounds\n", st->rounds);
   for (int i = 0; i < st->player_count; i++)
      printf("  %-16.*s %5d\n", STR_ARG(st->players[i].name),
             st->players[i].score);
}

static void usage(void) {
   fprintf(stderr,
           "usage: wobble_bot [-v] [-s server] [-r room] [-n bots] [-p players] [-t seconds]\n");
   exit(2);
}

int main(int argc, char **argv) {
   const char *server = "ws://localhost:4000";
   int timeout_s = 600;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-v") == 0) {
         verbose = true;
         continue;
      }
      if (i + 1 >= argc)
         usage();
      if (strcmp(argv[i], "-s") == 0)
         server = argv[++i];
      else if (strcmp(argv[i], "-r") == 0)
         room = argv[++i];
      else if (strcmp(argv[i], "-n") == 0)
         bot_count = atoi(argv[++i]);
      else if (strcmp(argv[i], "-p") == 0)
         wait_for = atoi(argv[++i]);
      else if (strcmp(argv[i], "-t") == 0)
         timeout_s = atoi(argv[++i]);
      else
         usage();
   }
   if (bot_count < 2 || bot_count > MAX_BOTS)
      usage();
   srand((unsigned)time(NULL));

   for (int i = 0; i < bot_count; i++) {
      bot *b = &bots[i];
      b->index = i;
      b->turn = -1;
      snprintf(b->name, sizeof(b->name), "bot%d", i + 1);
      b->g = game_new(send_frame, b);
      b->t = transport_connect(server, (transport_callbacks){
                                           .on_open = on_open,
                                           .on_message = on_message,
                                           .on_close = on_close,
                                           .user_data = (void *)(intptr_t)i,
                                       });
      if (!b->g || !b->t) {
         fprintf(stderr, "bad server URL: %s\n", server);
         return 1;
      }
   }

   u64 deadline = now_ms() + (u64)timeout_s * 1000;
   int result = -1;
   while (result < 0) {
      u64 now = now_ms();
      for (event *ev = take_events(), *next; ev; ev = next) {
         next = ev->next;
         bot *b = &bots[ev->bot];
         switch (ev->kind) {
         case EV_OPEN:
            game_connected(b->g, string_view(room), string_view(b->name), S(""),
                           now);
            break;
         case EV_MESSAGE:
            if (verbose && ev->bot == 0)
               printf("< %.200s\n", ev->data);
            game_on_message(b->g, ev->data, ev->len, now);
            break;
         case EV_CLOSE:
            fprintf(stderr, "%s: disconnected: %s\n", b->name, ev->data);
            result = 1;
            break;
         }
         free(ev);
      }

      for (int i = 0; i < bot_count && result < 0; i++) {
         bot *b = &bots[i];
         const game_state *st = game_get_state(b->g);
         if (st->phase == GAME_OFFLINE && st->error.len) {
            fprintf(stderr, "%s: " STR_FMT "\n", b->name, STR_ARG(st->error));
            result = 1;
         } else if (!play(b, now)) {
            result = 1;
         }
         game_tick(b->g, now);
         game_dirty(b->g);
      }

      if (result < 0 && game_get_state(bots[0].g)->phase == GAME_OVER) {
         print_scores(game_get_state(bots[0].g));
         result = 0;
      }
      if (result < 0 && now >= deadline) {
         fprintf(stderr, "timed out\n");
         result = 2;
      }
      nanosleep(&(struct timespec){0, 5 * 1000000}, NULL);
   }

   for (int i = 0; i < bot_count; i++) {
      transport_free(bots[i].t);
      game_free(bots[i].g);
   }
   return result;
}

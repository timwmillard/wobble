/* transport_web.c — the browser transport: transport.h on the browser's own
 * WebSocket through emscripten/websocket.h. Callbacks run on the browser's
 * main thread, between frames, so the app's Rtask_post hand-off still works.
 */

#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/websocket.h>

#define SOCKET_PATH "/socket/websocket?vsn=2.0.0"

struct transport {
   EMSCRIPTEN_WEBSOCKET_T ws;
   transport_callbacks cb;
   bool open;
   bool closed; // on_close already reported
};

static void report_close(transport *t, const char *reason) {
   t->open = false;
   if (t->closed)
      return;
   t->closed = true;
   if (t->cb.on_close)
      t->cb.on_close(reason, t->cb.user_data);
}

static bool on_open(int type, const EmscriptenWebSocketOpenEvent *e,
                    void *ud) {
   transport *t = ud;
   (void)type;
   (void)e;
   t->open = true;
   if (t->cb.on_open)
      t->cb.on_open(t->cb.user_data);
   return true;
}

static bool on_message(int type, const EmscriptenWebSocketMessageEvent *e,
                       void *ud) {
   transport *t = ud;
   (void)type;
   // Text frames arrive NUL-terminated; numBytes counts the NUL.
   if (e->isText && t->cb.on_message && e->numBytes > 0)
      t->cb.on_message((const char *)e->data, e->numBytes - 1,
                       t->cb.user_data);
   return true;
}

// The browser hides why a WebSocket failed; onerror is always followed by
// onclose, which carries the code.
static bool on_error(int type, const EmscriptenWebSocketErrorEvent *e,
                     void *ud) {
   (void)type;
   (void)e;
   (void)ud;
   return true;
}

static bool on_close(int type, const EmscriptenWebSocketCloseEvent *e,
                     void *ud) {
   transport *t = ud;
   (void)type;
   char text[160];
   if (e->reason[0])
      snprintf(text, sizeof(text), "%s", e->reason);
   else if (e->code == 1006)
      snprintf(text, sizeof(text), "couldn't reach the server");
   else
      snprintf(text, sizeof(text), "connection closed (%d)", e->code);
   report_close(t, text);
   return true;
}

transport *transport_connect(const char *server, transport_callbacks cb) {
   if (!emscripten_websocket_is_supported())
      return NULL;
   size_t n = strlen(server);
   while (n > 0 && server[n - 1] == '/')
      n--;
   if (!(n > 5 && strncmp(server, "ws://", 5) == 0) &&
       !(n > 6 && strncmp(server, "wss://", 6) == 0))
      return NULL;

   char *url = malloc(n + sizeof(SOCKET_PATH));
   transport *t = calloc(1, sizeof(*t));
   if (!url || !t) {
      free(url);
      free(t);
      return NULL;
   }
   memcpy(url, server, n);
   memcpy(url + n, SOCKET_PATH, sizeof(SOCKET_PATH));

   EmscriptenWebSocketCreateAttributes attrs;
   emscripten_websocket_init_create_attributes(&attrs);
   attrs.url = url;
   attrs.createOnMainThread = true;
   t->cb = cb;
   t->ws = emscripten_websocket_new(&attrs);
   free(url);
   if (t->ws <= 0) {
      free(t);
      return NULL;
   }
   emscripten_websocket_set_onopen_callback(t->ws, t, on_open);
   emscripten_websocket_set_onmessage_callback(t->ws, t, on_message);
   emscripten_websocket_set_onerror_callback(t->ws, t, on_error);
   emscripten_websocket_set_onclose_callback(t->ws, t, on_close);
   return t;
}

bool transport_send(transport *t, const char *data, size_t len) {
   if (!t || !t->open)
      return false;
   // send_utf8_text wants a C string; frames are not NUL-terminated.
   char *text = malloc(len + 1);
   if (!text)
      return false;
   memcpy(text, data, len);
   text[len] = '\0';
   EMSCRIPTEN_RESULT r = emscripten_websocket_send_utf8_text(t->ws, text);
   free(text);
   return r == EMSCRIPTEN_RESULT_SUCCESS;
}

void transport_free(transport *t) {
   if (!t)
      return;
   // delete drops the socket's handlers, so no callback runs after this.
   emscripten_websocket_close(t->ws, 1000, "");
   emscripten_websocket_delete(t->ws);
   free(t);
}

#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/net.h"

#define SOCKET_PATH "/socket/websocket?vsn=2.0.0"

struct transport {
   ws *ws;
   transport_callbacks cb;
   bool closed; // on_close already reported
};

static void on_open(ws *w, void *ud) {
   transport *t = ud;
   (void)w;
   if (t->cb.on_open)
      t->cb.on_open(t->cb.user_data);
}

static void on_message(ws *w, const char *data, size_t len, bool binary,
                       void *ud) {
   transport *t = ud;
   (void)w;
   if (!binary && t->cb.on_message)
      t->cb.on_message(data, len, t->cb.user_data);
}

// webc reports an error and then closes; pass on one reason, the error's.
static void report_close(transport *t, const char *reason) {
   if (t->closed)
      return;
   t->closed = true;
   if (t->cb.on_close)
      t->cb.on_close(reason, t->cb.user_data);
}

static void on_error(ws *w, const char *message, void *ud) {
   (void)w;
   report_close(ud, message ? message : "connection error");
}

static void on_close(ws *w, int code, const char *reason, void *ud) {
   (void)w;
   char text[160];
   if (reason && reason[0])
      snprintf(text, sizeof(text), "%s", reason);
   else
      snprintf(text, sizeof(text), "connection closed (%d)", code);
   report_close(ud, text);
}

transport *transport_connect(const char *server, transport_callbacks cb) {
   size_t n = strlen(server);
   while (n > 0 && server[n - 1] == '/')
      n--;
   char *url = malloc(n + sizeof(SOCKET_PATH));
   transport *t = calloc(1, sizeof(*t));
   if (!url || !t) {
      free(url);
      free(t);
      return NULL;
   }
   memcpy(url, server, n);
   memcpy(url + n, SOCKET_PATH, sizeof(SOCKET_PATH));

   t->cb = cb;
   t->ws = ws_connect(&(ws_desc){
       .url = url,
       .on_open = on_open,
       .on_message = on_message,
       .on_close = on_close,
       .on_error = on_error,
       .user_data = t,
   });
   free(url);
   if (!t->ws) {
      free(t);
      return NULL;
   }
   return t;
}

bool transport_send(transport *t, const char *data, size_t len) {
   return t && ws_send_text(t->ws, data, len);
}

void transport_free(transport *t) {
   if (!t)
      return;
   ws_free(t->ws);
   free(t);
}

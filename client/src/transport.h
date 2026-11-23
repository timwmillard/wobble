/* transport.h — one WebSocket to the game server. transport_native.c is on
 * webc (src/net); a browser build will add transport_web.c on
 * emscripten/websocket.h behind the same interface.
 *
 * Callbacks may run on another thread (webc's network thread on native).
 * Hand events to the thread that owns the game before touching it.
 */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct transport transport;

typedef struct {
   void (*on_open)(void *user_data);
   void (*on_message)(const char *data, size_t len, void *user_data);
   // Always the last callback. reason is never NULL.
   void (*on_close)(const char *reason, void *user_data);
   void *user_data;
} transport_callbacks;

// "ws://host:4000" or "wss://host" — the Phoenix socket path is added. NULL
// for a bad URL.
transport *transport_connect(const char *server, transport_callbacks cb);
// Thread-safe. False when not open.
bool transport_send(transport *t, const char *data, size_t len);
// Closes if open. No callback runs once it returns.
void transport_free(transport *t);

#endif

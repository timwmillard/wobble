/* link.h — room links: what a player types or pastes to join, and the link
 * they share. One format works everywhere: https://<host>/r/<code> opens the
 * web client when clicked, and the native app reads the server and the room
 * from it when pasted.
 *
 * Plain C, no I/O.
 */

#ifndef LINK_H
#define LINK_H

#include "base.h"

#define LINK_SERVER_MAX 256
#define LINK_ROOM_MAX 32
#define LINK_NAME_MAX 64

typedef struct {
   char server[LINK_SERVER_MAX]; // "ws://host:port" or "wss://host"; "" for
                                 // a bare code, which uses the default
   char room[LINK_ROOM_MAX];     // cleaned: lower case, digits, dashes
   char name[LINK_NAME_MAX];     // from ?name=, else ""
   bool is_link;                 // it had an address, not just a code
} link_target;

typedef enum {
   LINK_OK,
   LINK_BAD_ROOM,    // the code has characters a room code can't have
   LINK_NO_ROOM,     // an address, but no /r/<code> or ?room=
   LINK_BAD_ADDRESS, // not something we can connect to
} link_result;

// Reads any of:
//   abcd                              a code, on the default server
//   wobblegame.fly.dev/r/abcd         wss:// (ws:// for localhost and IPs)
//   https://wobblegame.fly.dev/r/abcd wss://wobblegame.fly.dev
//   http://localhost:4000/r/abcd      ws://localhost:4000
//   ws(s)://host/r/abcd, wobble://host/r/abcd, and ?room= / ?name=
link_result link_parse(const char *text, link_target *out);

// The server as transport_connect wants it, from what a player typed in the
// server field: "localhost:4000", "https://host", "wss://host/" and so on.
// False when it isn't an address.
bool link_server(const char *text, char *out, usize cap);

// Room codes are lower case letters, digits and dashes; upper case is
// lowered and spaces dropped, since people read codes aloud.
bool link_clean_room(const char *in, char *out, usize cap);

// The link to share: https://<host>/r/<room> for a wss:// server, http://
// for ws://. False when it doesn't fit or the server isn't ws(s)://.
bool link_invite(const char *server, const char *room, char *out, usize cap);

#endif

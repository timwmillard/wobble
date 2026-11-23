/* phx.h — Phoenix Channels V2 JSON framing. Every frame is an array:
 * [join_ref, ref, topic, event, payload]. No I/O and no channel state; the
 * game core (game.c) owns refs, joining and heartbeats.
 */

#ifndef PHX_H
#define PHX_H

#include "json.h"

typedef struct {
   string join_ref; // empty when null
   string ref;      // empty when null
   string topic;
   string event;
   json *payload; // never NULL after a successful parse
} phx_msg;

// Parses one frame into a. False if it isn't a 5-element array with string
// topic and event and an object payload.
bool phx_parse(arena *a, string text, phx_msg *out);

// Appends a frame to sb. join_ref and ref may be empty for null. payload is
// raw JSON (an object), written as is.
void phx_write(string_builder *sb, string join_ref, string ref, string topic,
               string event, string payload);

// For a phx_reply: payload.status ("ok" / "error") and payload.response.
string phx_reply_status(const phx_msg *m);
json *phx_reply_response(const phx_msg *m);

#endif

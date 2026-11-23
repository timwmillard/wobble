#include "phx.h"

static string ref_or_empty(json *j) {
   return (j && j->type == JSON_STRING) ? j->str : (string){0};
}

bool phx_parse(arena *a, string text, phx_msg *out) {
   json *j = json_parse(a, text);
   if (!j || j->type != JSON_ARRAY || j->len != 5)
      return false;
   json *topic = json_at(j, 2), *event = json_at(j, 3), *payload = json_at(j, 4);
   if (topic->type != JSON_STRING || event->type != JSON_STRING ||
       payload->type != JSON_OBJECT)
      return false;
   *out = (phx_msg){
       .join_ref = ref_or_empty(json_at(j, 0)),
       .ref = ref_or_empty(json_at(j, 1)),
       .topic = topic->str,
       .event = event->str,
       .payload = payload,
   };
   return true;
}

static void write_ref(string_builder *sb, string ref) {
   if (ref.len)
      json_write_string(sb, ref);
   else
      sb_append(sb, S("null"));
}

void phx_write(string_builder *sb, string join_ref, string ref, string topic,
               string event, string payload) {
   sb_push(sb, '[');
   write_ref(sb, join_ref);
   sb_push(sb, ',');
   write_ref(sb, ref);
   sb_push(sb, ',');
   json_write_string(sb, topic);
   sb_push(sb, ',');
   json_write_string(sb, event);
   sb_push(sb, ',');
   sb_append(sb, payload.len ? payload : S("{}"));
   sb_push(sb, ']');
}

string phx_reply_status(const phx_msg *m) {
   return json_str(json_get(m->payload, S("status")));
}

json *phx_reply_response(const phx_msg *m) {
   return json_get(m->payload, S("response"));
}

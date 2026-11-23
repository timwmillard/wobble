/* json.h — a small JSON parser into an arena, and a string writer. Enough for
 * the OpenAI API: objects, arrays, strings (escapes and \u surrogates),
 * numbers, true / false / null.
 */

#ifndef JSON_H
#define JSON_H

#include "base.h"

typedef enum {
   JSON_NULL,
   JSON_BOOL,
   JSON_NUMBER,
   JSON_STRING,
   JSON_ARRAY,
   JSON_OBJECT,
} json_type;

typedef struct json json;
struct json {
   json_type type;
   bool boolean;
   f64 number;
   string str;   // JSON_STRING, decoded to UTF-8
   json *items;  // JSON_ARRAY items, JSON_OBJECT values
   string *keys; // JSON_OBJECT keys, decoded
   usize len;
};

// Parses all of text. NULL if it isn't valid JSON or nests deeper than 64.
// Strings are copied into a, so text can go away.
json *json_parse(arena *a, string text);

// The member key of an object, or NULL if j is NULL, not an object or has no
// such member. Chains: json_get(json_get(j, S("error")), S("message")).
json *json_get(json *j, string key);
// Item i of an array, or NULL.
json *json_at(json *j, usize i);
// j's string, or an empty string if j isn't one.
string json_str(json *j);

// Appends s as a JSON string literal, quotes included.
void json_write_string(string_builder *sb, string s);

#endif

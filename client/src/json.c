#include "json.h"

#include <string.h>

#define MAX_DEPTH 64

typedef struct {
   arena *a;
   const char *p, *end;
   int depth;
} parser;

typedef LIST(json) json_list;
typedef LIST(string) key_list;

static void skip_space(parser *ps) {
   while (ps->p < ps->end &&
          (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
      ps->p++;
}

static bool literal(parser *ps, string word) {
   if ((usize)(ps->end - ps->p) < word.len ||
       memcmp(ps->p, word.data, word.len) != 0)
      return false;
   ps->p += word.len;
   return true;
}

static int hex4(const char *p) {
   int v = 0;
   for (int i = 0; i < 4; i++) {
      char c = p[i];
      int d = c >= '0' && c <= '9'   ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                     : -1;
      if (d < 0)
         return -1;
      v = v * 16 + d;
   }
   return v;
}

static void push_utf8(string_builder *sb, u32 cp) {
   if (cp < 0x80) {
      sb_push(sb, (char)cp);
   } else if (cp < 0x800) {
      sb_push(sb, (char)(0xC0 | (cp >> 6)));
      sb_push(sb, (char)(0x80 | (cp & 0x3F)));
   } else if (cp < 0x10000) {
      sb_push(sb, (char)(0xE0 | (cp >> 12)));
      sb_push(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
      sb_push(sb, (char)(0x80 | (cp & 0x3F)));
   } else {
      sb_push(sb, (char)(0xF0 | (cp >> 18)));
      sb_push(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
      sb_push(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
      sb_push(sb, (char)(0x80 | (cp & 0x3F)));
   }
}

// At the opening quote. Decodes into a string in the arena.
static bool parse_string(parser *ps, string *out) {
   ps->p++;
   string_builder sb = sb_init(ps->a);
   while (ps->p < ps->end && *ps->p != '"') {
      unsigned char c = (unsigned char)*ps->p;
      if (c < 0x20)
         return false;
      if (c != '\\') {
         // Copy the run up to the next quote or escape in one go.
         const char *run = ps->p;
         while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\' &&
                (unsigned char)*ps->p >= 0x20)
            ps->p++;
         sb_append_from_bytes(&sb, run, (usize)(ps->p - run));
         continue;
      }
      if (ps->end - ps->p < 2)
         return false;
      char e = ps->p[1];
      ps->p += 2;
      switch (e) {
      case '"':
      case '\\':
      case '/':
         sb_push(&sb, e);
         break;
      case 'b':
         sb_push(&sb, '\b');
         break;
      case 'f':
         sb_push(&sb, '\f');
         break;
      case 'n':
         sb_push(&sb, '\n');
         break;
      case 'r':
         sb_push(&sb, '\r');
         break;
      case 't':
         sb_push(&sb, '\t');
         break;
      case 'u': {
         if (ps->end - ps->p < 4)
            return false;
         int cp = hex4(ps->p);
         if (cp < 0)
            return false;
         ps->p += 4;
         if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
             ps->p[0] == '\\' && ps->p[1] == 'u') {
            int lo = hex4(ps->p + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
               cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
               ps->p += 6;
            }
         }
         if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = 0xFFFD; // a lone surrogate
         push_utf8(&sb, (u32)cp);
         break;
      }
      default:
         return false;
      }
   }
   if (ps->p >= ps->end)
      return false;
   ps->p++;
   *out = sb_string(&sb);
   return true;
}

static bool parse_number(parser *ps, f64 *out) {
   const char *start = ps->p;
   if (ps->p < ps->end && *ps->p == '-')
      ps->p++;
   if (ps->p >= ps->end || !(*ps->p >= '0' && *ps->p <= '9'))
      return false;
   while (ps->p < ps->end &&
          ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' || *ps->p == 'e' ||
           *ps->p == 'E' || *ps->p == '+' || *ps->p == '-'))
      ps->p++;
   char buf[64];
   usize n = (usize)(ps->p - start);
   if (n >= sizeof(buf))
      return false;
   memcpy(buf, start, n);
   buf[n] = '\0';
   char *end;
   *out = strtod(buf, &end);
   return end == buf + n;
}

static bool parse_value(parser *ps, json *out);

static bool parse_array(parser *ps, json *out) {
   ps->p++;
   json_list items = list_init(ps->a);
   skip_space(ps);
   if (ps->p < ps->end && *ps->p == ']') {
      ps->p++;
   } else {
      for (;;) {
         json item;
         if (!parse_value(ps, &item))
            return false;
         list_append(&items, item);
         skip_space(ps);
         if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
         }
         if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            break;
         }
         return false;
      }
   }
   out->type = JSON_ARRAY;
   out->items = items.items;
   out->len = items.len;
   return true;
}

static bool parse_object(parser *ps, json *out) {
   ps->p++;
   json_list values = list_init(ps->a);
   key_list keys = list_init(ps->a);
   skip_space(ps);
   if (ps->p < ps->end && *ps->p == '}') {
      ps->p++;
   } else {
      for (;;) {
         skip_space(ps);
         string key;
         if (ps->p >= ps->end || *ps->p != '"' || !parse_string(ps, &key))
            return false;
         skip_space(ps);
         if (ps->p >= ps->end || *ps->p != ':')
            return false;
         ps->p++;
         json value;
         if (!parse_value(ps, &value))
            return false;
         list_append(&keys, key);
         list_append(&values, value);
         skip_space(ps);
         if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
         }
         if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
         }
         return false;
      }
   }
   out->type = JSON_OBJECT;
   out->items = values.items;
   out->keys = keys.items;
   out->len = values.len;
   return true;
}

static bool parse_value(parser *ps, json *out) {
   memset(out, 0, sizeof(*out));
   skip_space(ps);
   if (ps->p >= ps->end)
      return false;
   switch (*ps->p) {
   case '{':
   case '[': {
      if (++ps->depth > MAX_DEPTH)
         return false;
      bool ok = *ps->p == '{' ? parse_object(ps, out) : parse_array(ps, out);
      ps->depth--;
      return ok;
   }
   case '"':
      out->type = JSON_STRING;
      return parse_string(ps, &out->str);
   case 't':
      out->type = JSON_BOOL;
      out->boolean = true;
      return literal(ps, S("true"));
   case 'f':
      out->type = JSON_BOOL;
      return literal(ps, S("false"));
   case 'n':
      out->type = JSON_NULL;
      return literal(ps, S("null"));
   default:
      out->type = JSON_NUMBER;
      return parse_number(ps, &out->number);
   }
}

json *json_parse(arena *a, string text) {
   parser ps = {a, text.data, text.data + text.len, 0};
   json *root = arena_alloc(a, sizeof(*root));
   if (!parse_value(&ps, root))
      return NULL;
   skip_space(&ps);
   return ps.p == ps.end ? root : NULL;
}

json *json_get(json *j, string key) {
   if (!j || j->type != JSON_OBJECT)
      return NULL;
   for (usize i = 0; i < j->len; i++) {
      if (string_eq(j->keys[i], key))
         return &j->items[i];
   }
   return NULL;
}

json *json_at(json *j, usize i) {
   if (!j || j->type != JSON_ARRAY || i >= j->len)
      return NULL;
   return &j->items[i];
}

string json_str(json *j) {
   return j && j->type == JSON_STRING ? j->str : S("");
}

void json_write_string(string_builder *sb, string s) {
   sb_push(sb, '"');
   for (usize i = 0; i < s.len; i++) {
      unsigned char c = (unsigned char)s.data[i];
      switch (c) {
      case '"':
         sb_append(sb, S("\\\""));
         break;
      case '\\':
         sb_append(sb, S("\\\\"));
         break;
      case '\n':
         sb_append(sb, S("\\n"));
         break;
      case '\r':
         sb_append(sb, S("\\r"));
         break;
      case '\t':
         sb_append(sb, S("\\t"));
         break;
      default:
         if (c < 0x20)
            sb_appendf(sb, "\\u%04x", c);
         else
            sb_push(sb, (char)c);
      }
   }
   sb_push(sb, '"');
}

/* link.c — room links; see link.h. */

#include "link.h"

#include <stdio.h>
#include <string.h>

static bool is_space(char ch) {
   return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static char lower(char ch) {
   return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
}

static bool is_digit(char ch) {
   return ch >= '0' && ch <= '9';
}

static bool copy_to(string s, char *out, usize cap) {
   if (s.len + 1 > cap)
      return false;
   memcpy(out, s.data, s.len);
   out[s.len] = '\0';
   return true;
}

bool link_clean_room(const char *in, char *out, usize cap) {
   usize n = 0;
   for (const char *p = in; *p; p++) {
      char ch = lower(*p);
      if (is_space(ch))
         continue;
      if (!((ch >= 'a' && ch <= 'z') || is_digit(ch) || ch == '-'))
         return false;
      if (n + 1 >= cap)
         return false;
      out[n++] = ch;
   }
   out[n] = '\0';
   return n > 0;
}

// Without a scheme we guess: a dev server on this machine or the LAN has no
// TLS, anything with a real name does.
static bool host_is_local(string host) {
   if (string_starts_with(host, S("["))) // [::1]:4000
      return true;
   string name;
   string_cut(host, S(":"), &name, NULL); // drop the port
   if (string_eq_nocase(name, S("localhost")))
      return true;
   if (name.len > 6 && string_eq_nocase(string_suffix(name, 6), S(".local")))
      return true;
   for (usize i = 0; i < name.len; i++) { // an IPv4 address
      if (!is_digit(name.data[i]) && name.data[i] != '.')
         return false;
   }
   return name.len > 0;
}

static bool host_ok(string host) {
   if (!host.len)
      return false;
   for (usize i = 0; i < host.len; i++) {
      char ch = lower(host.data[i]);
      if (!((ch >= 'a' && ch <= 'z') || is_digit(ch) || ch == '.' ||
            ch == '-' || ch == '_' || ch == ':' || ch == '[' || ch == ']'))
         return false;
   }
   return true;
}

// Splits "scheme://host/rest" into the ws(s):// server and the rest (the
// path, query and fragment, possibly empty).
static bool split_address(string text, char *server, usize cap,
                          string *rest) {
   int secure = -1; // -1: guess from the host
   static const struct {
      const char *prefix;
      int secure;
   } schemes[] = {
       {"https://", 1}, {"http://", 0}, {"wss://", 1},
       {"ws://", 0},    {"wobble://", -1},
   };
   for (usize i = 0; i < ARRAY_SIZE(schemes); i++) {
      string prefix = string_view(schemes[i].prefix);
      if (string_starts_with_nocase(text, prefix)) {
         text = string_slice(text, (isize)prefix.len, (isize)text.len);
         secure = schemes[i].secure;
         break;
      }
   }
   isize end = string_index_of_any(text, S("/?#"));
   if (end < 0)
      end = (isize)text.len;
   string host = string_prefix(text, (usize)end);
   if (!host_ok(host))
      return false;
   if (secure < 0)
      secure = !host_is_local(host);
   *rest = string_slice(text, end, (isize)text.len);
   int n = snprintf(server, cap, "%s://" STR_FMT, secure ? "wss" : "ws",
                    STR_ARG(host));
   if (n < 0 || (usize)n >= cap)
      return false;
   // Host names don't care about case; keep the server tidy.
   for (char *p = server; *p; p++)
      *p = lower(*p);
   return true;
}

static int hex(char ch) {
   ch = lower(ch);
   if (is_digit(ch))
      return ch - '0';
   if (ch >= 'a' && ch <= 'f')
      return ch - 'a' + 10;
   return -1;
}

// A query value: %XX and '+' for space.
static void url_decode(string s, char *out, usize cap) {
   usize n = 0;
   for (usize i = 0; i < s.len && n + 1 < cap; i++) {
      char ch = s.data[i];
      if (ch == '+') {
         ch = ' ';
      } else if (ch == '%' && i + 2 < s.len && hex(s.data[i + 1]) >= 0 &&
                 hex(s.data[i + 2]) >= 0) {
         ch = (char)(hex(s.data[i + 1]) * 16 + hex(s.data[i + 2]));
         i += 2;
      }
      out[n++] = ch;
   }
   out[n] = '\0';
}

// The value of key in "a=1&b=2", or false.
static bool query_get(string query, string key, string *value) {
   string pair, k;
   while (string_split_next(&query, S("&"), &pair)) {
      if (string_cut(pair, S("="), &k, value) && string_eq(k, key))
         return true;
   }
   return false;
}

link_result link_parse(const char *text, link_target *out) {
   memset(out, 0, sizeof(*out));
   string s = string_trim(string_view(text ? text : ""));

   if (string_index_of_any(s, S("/:.?")) < 0) {
      char code[LINK_ROOM_MAX * 2];
      if (!copy_to(s, code, sizeof(code)))
         return LINK_BAD_ROOM;
      return link_clean_room(code, out->room, sizeof(out->room))
                 ? LINK_OK
                 : LINK_BAD_ROOM;
   }

   out->is_link = true;
   string rest;
   if (!split_address(s, out->server, sizeof(out->server), &rest))
      return LINK_BAD_ADDRESS;

   // rest is "/path?query#fragment"
   string path, query, code, value;
   string_cut(rest, S("#"), &rest, NULL);
   string_cut(rest, S("?"), &path, &query);
   if (string_cut(path, S("/r/"), NULL, &code)) // the segment after /r/
      string_cut(code, S("/"), &code, NULL);
   if (!code.len && query_get(query, S("room"), &value))
      code = value;
   if (query_get(query, S("name"), &value))
      url_decode(value, out->name, sizeof(out->name));

   if (!code.len)
      return LINK_NO_ROOM;
   char raw[LINK_ROOM_MAX * 2];
   url_decode(code, raw, sizeof(raw));
   return link_clean_room(raw, out->room, sizeof(out->room)) ? LINK_OK
                                                              : LINK_BAD_ROOM;
}

bool link_server(const char *text, char *out, usize cap) {
   string s = string_trim(string_view(text ? text : ""));
   string rest;
   return s.len && split_address(s, out, cap, &rest);
}

bool link_invite(const char *server, const char *room, char *out, usize cap) {
   string s = string_view(server), host;
   const char *scheme;
   if (string_starts_with_nocase(s, S("wss://")))
      scheme = "https://";
   else if (string_starts_with_nocase(s, S("ws://")))
      scheme = "http://";
   else
      return false;
   string_cut(s, S("://"), NULL, &s);
   string_cut(s, S("/"), &host, NULL);
   int n = snprintf(out, cap, "%s" STR_FMT "/r/%s", scheme, STR_ARG(host),
                    room);
   return n > 0 && (usize)n < cap;
}

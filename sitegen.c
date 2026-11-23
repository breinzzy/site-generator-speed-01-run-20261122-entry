/* sitegen: native C port of makefun-site 1.0.0 (solve.js), byte-exact.
 * Reads <inputDir>/cases.json, writes <outputDir>/answers.json.
 * Strings are UTF-8 byte strings (WTF-8 for lone surrogates from \u escapes);
 * JS string semantics (UTF-16 code units) are reproduced where they matter.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "slugtab.h"

/* ------------------------------------------------------------------ arena */
typedef struct { char **blocks; size_t nblocks, cap; size_t used; } Arena;
#define ARENA_BLOCK (1u << 20)
static void arena_init(Arena *a) { memset(a, 0, sizeof *a); }
static void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (a->nblocks == 0 || a->used + n > ARENA_BLOCK) {
    if (a->nblocks == a->cap) {
      a->cap = a->cap ? a->cap * 2 : 8;
      a->blocks = realloc(a->blocks, a->cap * sizeof(char *));
      if (!a->blocks) exit(1);
    }
    size_t sz = n > ARENA_BLOCK ? n : ARENA_BLOCK;
    a->blocks[a->nblocks++] = malloc(sz);
    if (!a->blocks[a->nblocks - 1]) exit(1);
    a->used = 0;
    if (n > ARENA_BLOCK) { void *p = a->blocks[a->nblocks - 1]; a->used = n; return p; }
  }
  void *p = a->blocks[a->nblocks - 1] + a->used;
  a->used += n;
  return p;
}
static void arena_reset(Arena *a) {
  for (size_t i = 0; i < a->nblocks; i++) free(a->blocks[i]);
  free(a->blocks);
  memset(a, 0, sizeof *a);
}

typedef struct { const char *s; size_t len; } Str;
static Str str_make(const char *s, size_t len) { Str r; r.s = s; r.len = len; return r; }
static Str str_c(const char *s) { return str_make(s, strlen(s)); }
static int str_eq(Str a, Str b) { return a.len == b.len && (a.len == 0 || memcmp(a.s, b.s, a.len) == 0); }
static int str_eq_c(Str a, const char *b) { size_t n = strlen(b); return a.len == n && memcmp(a.s, b, n) == 0; }
static Str arena_str(Arena *a, const char *s, size_t len) {
  char *p = arena_alloc(a, len + 1);
  if (len) memcpy(p, s, len);
  p[len] = 0;
  return str_make(p, len);
}
static Str arena_str_c(Arena *a, const char *s) { return arena_str(a, s, strlen(s)); }
static Str arena_str_cat(Arena *a, Str x, Str y) {
  char *p = arena_alloc(a, x.len + y.len + 1);
  if (x.len) memcpy(p, x.s, x.len);
  if (y.len) memcpy(p + x.len, y.s, y.len);
  p[x.len + y.len] = 0;
  return str_make(p, x.len + y.len);
}
static Str arena_str_cat3(Arena *a, Str x, Str y, Str z) {
  char *p = arena_alloc(a, x.len + y.len + z.len + 1);
  size_t o = 0;
  if (x.len) memcpy(p + o, x.s, x.len); o += x.len;
  if (y.len) memcpy(p + o, y.s, y.len); o += y.len;
  if (z.len) memcpy(p + o, z.s, z.len); o += z.len;
  p[o] = 0;
  return str_make(p, o);
}

/* ------------------------------------------------------------------ StrBuf */
typedef struct { char *buf; size_t len, cap; } StrBuf;
static void sb_init(StrBuf *b) { b->buf = NULL; b->len = 0; b->cap = 0; }
static void sb_reserve(StrBuf *b, size_t extra) {
  if (b->len + extra + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    b->buf = realloc(b->buf, nc);
    if (!b->buf) exit(1);
    b->cap = nc;
  }
}
static void sb_putn(StrBuf *b, const char *s, size_t n) {
  if (!n) return;
  sb_reserve(b, n);
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = 0;
}
static void sb_put(StrBuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_puts(StrBuf *b, Str s) { sb_putn(b, s.s, s.len); }
static void sb_putc(StrBuf *b, char c) { sb_reserve(b, 1); b->buf[b->len++] = c; b->buf[b->len] = 0; }
static void sb_free(StrBuf *b) { free(b->buf); b->buf = NULL; b->len = b->cap = 0; }
static Str sb_to_arena(Arena *a, StrBuf *b) { Str r = arena_str(a, b->buf ? b->buf : "", b->len); sb_free(b); return r; }

/* ------------------------------------------------------------------ UTF-8 */
/* Decode one code point at byte offset i (len = string length). Returns bytes
 * consumed (>=1). Lone surrogates arrive as WTF-8 (ED A0..ED BF) and decode
 * to their surrogate value. Invalid bytes decode as the byte itself. */
static size_t utf8_decode(const char *s, size_t len, size_t i, unsigned *cp) {
  if (i >= len) { *cp = 0; return 0; }
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) { *cp = c; return 1; }
  if (c >= 0xC2 && c <= 0xDF && i + 1 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
    *cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F); return 2;
  }
  if (c >= 0xE0 && c <= 0xEF && i + 2 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
      ((unsigned char)s[i + 2] & 0xC0) == 0x80) {
    *cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4 && i + 3 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
      ((unsigned char)s[i + 2] & 0xC0) == 0x80 && ((unsigned char)s[i + 3] & 0xC0) == 0x80) {
    *cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) |
          (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
    return 4;
  }
  *cp = c; return 1;
}
static size_t utf8_encode(char out[4], unsigned cp) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
/* JS string length = number of UTF-16 code units. */
static size_t js_len(Str s) {
  size_t n = 0, i = 0;
  while (i < s.len) { unsigned cp; size_t k = utf8_decode(s.s, s.len, i, &cp); i += k; n += (cp > 0xFFFF) ? 2 : 1; }
  return n;
}
/* JS whitespace: \s and String trim set (U+0085 is NOT in either). */
static int is_js_ws(unsigned cp) {
  return cp == 9 || cp == 10 || cp == 11 || cp == 12 || cp == 13 || cp == 32 || cp == 0xA0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
static int is_uni_ws(unsigned cp) {
  return cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
static int is_ws_code(unsigned cp) {
  return cp == 32 || cp == 9 || cp == 10 || cp == 13 || cp == 12 || cp == 11 || is_uni_ws(cp);
}
static int has_js_ws(Str s) {
  size_t i = 0;
  while (i < s.len) { unsigned cp; size_t k = utf8_decode(s.s, s.len, i, &cp); i += k; if (is_js_ws(cp)) return 1; }
  return 0;
}
/* JS trim (both ends) on a byte range. */
static void trim_js_range(const char *s, size_t a, size_t b, size_t *ra, size_t *rb) {
  while (a < b) { unsigned cp; size_t k = utf8_decode(s, b, a, &cp); if (!is_js_ws(cp)) break; a += k; }
  while (b > a) {
    /* find start of last code point */
    size_t j = b - 1;
    while (j > a && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    unsigned cp; utf8_decode(s, b, j, &cp);
    if (!is_js_ws(cp)) break;
    b = j;
  }
  *ra = a; *rb = b;
}
/* Compare JS strings by UTF-16 code units (byCodeUnits). */
typedef struct { const char *s; size_t len, pos; int pend; unsigned pend_cp; } UnitIter;
static int unit_next(UnitIter *it) {
  if (it->pend) { it->pend = 0; return (int)(0xDC00 + (it->pend_cp & 0x3FF)); }
  if (it->pos >= it->len) return -1;
  unsigned cp; size_t k = utf8_decode(it->s, it->len, it->pos, &cp);
  it->pos += k;
  if (cp > 0xFFFF) { it->pend = 1; it->pend_cp = cp; return (int)(0xD800 + ((cp - 0x10000) >> 10)); }
  return (int)cp;
}
static int cmp_js_str(Str a, Str b) {
  /* ASCII fast path: byte order == code-unit order when both are pure ASCII. */
  size_t i = 0;
  while (i < a.len && (unsigned char)a.s[i] < 0x80) i++;
  int a_ascii = i == a.len;
  i = 0;
  while (i < b.len && (unsigned char)b.s[i] < 0x80) i++;
  int b_ascii = i == b.len;
  if (a_ascii && b_ascii) {
    size_t m = a.len < b.len ? a.len : b.len;
    int c = m ? memcmp(a.s, b.s, m) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
  }
  UnitIter ia = {a.s, a.len, 0, 0, 0}, ib = {b.s, b.len, 0, 0, 0};
  for (;;) {
    int ua = unit_next(&ia), ub = unit_next(&ib);
    if (ua != ub) return ua < ub ? -1 : 1;
    if (ua == -1) return 0;
  }
}
static int starts_with(Str s, const char *prefix) {
  size_t n = strlen(prefix);
  return s.len >= n && memcmp(s.s, prefix, n) == 0;
}
static int ends_with(Str s, const char *suffix) {
  size_t n = strlen(suffix);
  return s.len >= n && memcmp(s.s + s.len - n, suffix, n) == 0;
}

/* ------------------------------------------------------------------ JSON */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
  JType t;
  int b;
  double num;
  Str raw_num;
  Str str;
  JVal **items; size_t nitems, capitems;
  Str *keys; JVal **vals; size_t n, cap;
};
typedef struct { const char *s; size_t len, pos; Arena *a; int err; } JParser;
static void jp_ws(JParser *p) {
  while (p->pos < p->len) {
    char c = p->s[p->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
    else break;
  }
}
static JVal *jp_new(JParser *p, JType t) {
  JVal *v = arena_alloc(p->a, sizeof(JVal));
  memset(v, 0, sizeof *v);
  v->t = t;
  return v;
}
static JVal *jp_value(JParser *p);
static int hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i]; v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}
static Str jp_string_raw(JParser *p) {
  /* p->s[p->pos] == '"'. Returns decoded string (UTF-8 / WTF-8). */
  StrBuf b; sb_init(&b);
  p->pos++;
  while (p->pos < p->len) {
    unsigned char c = (unsigned char)p->s[p->pos];
    if (c == '"') { p->pos++; Str r = sb_to_arena(p->a, &b); return r; }
    if (c == '\\') {
      p->pos++;
      if (p->pos >= p->len) break;
      char e = p->s[p->pos];
      char out4[4]; size_t n;
      switch (e) {
        case '"': sb_putc(&b, '"'); p->pos++; break;
        case '\\': sb_putc(&b, '\\'); p->pos++; break;
        case '/': sb_putc(&b, '/'); p->pos++; break;
        case 'b': sb_putc(&b, '\b'); p->pos++; break;
        case 'f': sb_putc(&b, '\f'); p->pos++; break;
        case 'n': sb_putc(&b, '\n'); p->pos++; break;
        case 'r': sb_putc(&b, '\r'); p->pos++; break;
        case 't': sb_putc(&b, '\t'); p->pos++; break;
        case 'u': {
          unsigned cp;
          if (p->pos + 4 >= p->len || !hex4(p->s + p->pos + 1, &cp)) { p->err = 1; sb_free(&b); return str_c(""); }
          p->pos += 5;
          if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 1 < p->len && p->s[p->pos] == '\\' &&
              p->s[p->pos + 1] == 'u' && p->pos + 5 < p->len) {
            unsigned lo;
            if (hex4(p->s + p->pos + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p->pos += 6;
            }
          }
          n = utf8_encode(out4, cp);
          sb_putn(&b, out4, n);
          break;
        }
        default: p->err = 1; sb_free(&b); return str_c("");
      }
      continue;
    }
    /* raw run: copy everything up to the next '"' or '\\' in one go */
    size_t run = p->pos;
    while (run < p->len && p->s[run] != '"' && p->s[run] != '\\') run++;
    sb_putn(&b, p->s + p->pos, run - p->pos);
    p->pos = run;
  }
  p->err = 1;
  sb_free(&b);
  return str_c("");
}
static JVal *jp_value(JParser *p) {
  jp_ws(p);
  if (p->pos >= p->len) { p->err = 1; return NULL; }
  char c = p->s[p->pos];
  if (c == '{') {
    JVal *v = jp_new(p, J_OBJ);
    p->pos++;
    jp_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return v; }
    for (;;) {
      jp_ws(p);
      if (p->pos >= p->len || p->s[p->pos] != '"') { p->err = 1; return v; }
      Str key = jp_string_raw(p);
      if (p->err) return v;
      jp_ws(p);
      if (p->pos >= p->len || p->s[p->pos] != ':') { p->err = 1; return v; }
      p->pos++;
      JVal *val = jp_value(p);
      if (p->err) return v;
      /* duplicate key: replace value, keep first position (JSON.parse) */
      size_t found = (size_t)-1;
      for (size_t i = 0; i < v->n; i++) if (str_eq(v->keys[i], key)) { found = i; break; }
      if (found != (size_t)-1) v->vals[found] = val;
      else {
        if (v->n == v->cap) {
          v->cap = v->cap ? v->cap * 2 : 8;
          v->keys = realloc(v->keys, v->cap * sizeof(Str));
          v->vals = realloc(v->vals, v->cap * sizeof(JVal *));
          if (!v->keys || !v->vals) exit(1);
        }
        v->keys[v->n] = key;
        v->vals[v->n] = val;
        v->n++;
      }
      jp_ws(p);
      if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
      if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; break; }
      p->err = 1;
      return v;
    }
    return v;
  }
  if (c == '[') {
    JVal *v = jp_new(p, J_ARR);
    p->pos++;
    jp_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return v; }
    for (;;) {
      JVal *it = jp_value(p);
      if (p->err) return v;
      if (v->nitems == v->capitems) {
        v->capitems = v->capitems ? v->capitems * 2 : 8;
        JVal **ni = malloc(v->capitems * sizeof(JVal *));
        if (v->items) memcpy(ni, v->items, v->nitems * sizeof(JVal *));
        free(v->items);
        v->items = ni;
      }
      v->items[v->nitems++] = it;
      jp_ws(p);
      if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
      if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; break; }
      p->err = 1;
      return v;
    }
    return v;
  }
  if (c == '"') {
    JVal *v = jp_new(p, J_STR);
    v->str = jp_string_raw(p);
    return v;
  }
  if (c == 't' && p->len - p->pos >= 4 && memcmp(p->s + p->pos, "true", 4) == 0) {
    JVal *v = jp_new(p, J_BOOL); v->b = 1; p->pos += 4; return v;
  }
  if (c == 'f' && p->len - p->pos >= 5 && memcmp(p->s + p->pos, "false", 5) == 0) {
    JVal *v = jp_new(p, J_BOOL); v->b = 0; p->pos += 5; return v;
  }
  if (c == 'n' && p->len - p->pos >= 4 && memcmp(p->s + p->pos, "null", 4) == 0) {
    JVal *v = jp_new(p, J_NULL); p->pos += 4; return v;
  }
  /* number */
  size_t start = p->pos;
  if (p->pos < p->len && p->s[p->pos] == '-') p->pos++;
  while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
  if (p->pos < p->len && p->s[p->pos] == '.') {
    p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
  }
  if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
    p->pos++;
    if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
  }
  if (p->pos == start) { p->err = 1; return NULL; }
  JVal *v = jp_new(p, J_NUM);
  v->raw_num = arena_str(p->a, p->s + start, p->pos - start);
  v->num = strtod(v->raw_num.s, NULL);
  return v;
}
static JVal *json_parse(Arena *a, const char *s, size_t len, int *err) {
  JParser p = {s, len, 0, a, 0};
  JVal *v = jp_value(&p);
  if (!p.err) { jp_ws(&p); if (p.pos != p.len) p.err = 1; }
  *err = p.err;
  return v;
}
static JVal *jobj_get(JVal *o, const char *key) {
  if (!o || o->t != J_OBJ) return NULL;
  for (size_t i = 0; i < o->n; i++) if (str_eq_c(o->keys[i], key)) return o->vals[i];
  return NULL;
}

/* ------------------------------------------------------- JS number -> text */
/* ECMAScript Number::toString for a finite double, via shortest round-trip. */
static void fmt_js_number(double d, StrBuf *out) {
  if (isnan(d)) { sb_put(out, "NaN"); return; }
  if (isinf(d)) { sb_put(out, d < 0 ? "-Infinity" : "Infinity"); return; }
  if (d == 0) { sb_put(out, "0"); return; }
  if (d == trunc(d) && fabs(d) < 1e21) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.0f", d);
    sb_put(out, tmp);
    return;
  }
  /* shortest digits: try precision 1..17 with %e, keep first that round-trips */
  char digits[32]; int ndig = 0, exp10 = 0; int found = 0;
  for (int prec = 1; prec <= 17 && !found; prec++) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.*e", prec - 1, d);
    /* tmp like "-1.234e+05" or "3.000e-07" */
    char *epos = strchr(tmp, 'e');
    int ex = atoi(epos + 1);
    int nd = 0;
    for (char *q = tmp; q < epos; q++) if (*q >= '0' && *q <= '9') digits[nd++] = *q;
    digits[nd] = 0;
    /* round-trip check: build decimal string and strtod it */
    char dec[80];
    if (nd == 1) snprintf(dec, sizeof dec, "%c e%d", digits[0], ex);
    else snprintf(dec, sizeof dec, "%c.%s e%d", digits[0], digits + 1, ex);
    /* strip the space, keep the sign */
    char dec2[80]; int w = 0;
    if (d < 0) dec2[w++] = '-';
    for (char *q = dec; *q; q++) if (*q != ' ') dec2[w++] = *q;
    dec2[w] = 0;
    if (strtod(dec2, NULL) == d) { ndig = nd; exp10 = ex + 1; found = 1; }
  }
  if (!found) { ndig = 17; exp10 = 0; }
  int n = exp10, k = ndig;
  int neg = d < 0;
  if (neg) sb_putc(out, '-');
  if (k <= n && n <= 21) {
    sb_putn(out, digits, k);
    for (int i = 0; i < n - k; i++) sb_putc(out, '0');
  } else if (0 < n && n <= 21) {
    sb_putn(out, digits, n);
    sb_putc(out, '.');
    sb_putn(out, digits + n, k - n);
  } else if (-6 < n && n <= 0) {
    sb_put(out, "0.");
    for (int i = 0; i < -n; i++) sb_putc(out, '0');
    sb_putn(out, digits, k);
  } else {
    sb_putc(out, digits[0]);
    if (k > 1) { sb_putc(out, '.'); sb_putn(out, digits + 1, k - 1); }
    char eb[16];
    snprintf(eb, sizeof eb, "e%+d", n - 1);
    sb_put(out, eb);
  }
}

/* ------------------------------------------------------------ JSON output */
/* JSON.stringify escaping: " \ and <0x20 (\b \f \n \r \t short, else \u00xx);
 * U+2028/U+2029 NOT escaped; lone surrogates (WTF-8) as \udXXX. */
static const char HEXL[] = "0123456789abcdef";
/* SWAR: does 8-byte word w contain a byte that needs JSON escaping
 * (<0x20, '"', '\\', >=0x80)? */
static inline int word_needs_escape(uint64_t w) {
  if (w & 0x8080808080808080ULL) return 1; /* >= 0x80 */
  /* hasless(w, 0x20): some byte < 0x20 */
  if (((w - 0x2020202020202020ULL) & ~w & 0x8080808080808080ULL) != 0) return 1;
  /* haszero(w ^ 0x22..): some byte == '"' */
  uint64_t x = w ^ 0x2222222222222222ULL;
  if (((x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL) != 0) return 1;
  /* haszero(w ^ 0x5c..): some byte == '\\' */
  x = w ^ 0x5C5C5C5C5C5C5C5CULL;
  if (((x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL) != 0) return 1;
  return 0;
}
static inline int byte_needs_escape(unsigned char c) {
  return c < 0x20 || c == '"' || c == '\\' || c >= 0x80;
}

static void json_escape_str(StrBuf *out, Str s) {
  /* Direct buffer writes with a local pointer: avoids a function call per
   * run/escape when serializing megabytes of HTML. */
  sb_reserve(out, 64);
  char *w = out->buf + out->len;
  char *cap_end = out->buf + out->cap;
#define JE_NEED(n) do { \
    if ((size_t)(cap_end - w) < (size_t)(n)) { \
      out->len = (size_t)(w - out->buf); \
      sb_reserve(out, (n) + 4096); \
      w = out->buf + out->len; cap_end = out->buf + out->cap; \
    } } while (0)
#define JE_PUTC(c) do { JE_NEED(1); *w++ = (char)(c); } while (0)
#define JE_PUTN(p, n) do { \
    const char *_p = (p); size_t _n = (n); \
    while (_n) { \
      JE_NEED(_n); \
      size_t _room = (size_t)(cap_end - w); \
      size_t _take = _n < _room ? _n : _room; \
      memcpy(w, _p, _take); w += _take; _p += _take; _n -= _take; \
    } } while (0)
  JE_PUTC('"');
  size_t i = 0;
  while (i < s.len) {
    /* fast run: SWAR over 8-byte words, then exact byte position */
    size_t run = i;
    while (run + 8 <= s.len) {
      uint64_t w;
      memcpy(&w, s.s + run, 8);
      if (word_needs_escape(w)) break;
      run += 8;
    }
    while (run < s.len && !byte_needs_escape((unsigned char)s.s[run])) run++;
    if (run > i) { JE_PUTN(s.s + i, run - i); i = run; }
    if (i >= s.len) break;
    unsigned char c = (unsigned char)s.s[i];
    if (c < 0x80) {
      switch (c) {
        case '"': JE_PUTN("\\\"", 2); i++; break;
        case '\\': JE_PUTN("\\\\", 2); i++; break;
        case '\b': JE_PUTN("\\b", 2); i++; break;
        case '\f': JE_PUTN("\\f", 2); i++; break;
        case '\n': JE_PUTN("\\n", 2); i++; break;
        case '\r': JE_PUTN("\\r", 2); i++; break;
        case '\t': JE_PUTN("\\t", 2); i++; break;
        default:
          JE_NEED(6);
          *w++ = '\\'; *w++ = 'u'; *w++ = '0'; *w++ = '0';
          *w++ = HEXL[c >> 4]; *w++ = HEXL[c & 15];
          i++;
      }
      continue;
    }
    size_t k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    if (i + k > s.len) k = s.len - i;
    unsigned cp;
    utf8_decode(s.s, s.len, i, &cp);
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      JE_NEED(6);
      *w++ = '\\'; *w++ = 'u';
      *w++ = HEXL[(cp >> 12) & 15]; *w++ = HEXL[(cp >> 8) & 15];
      *w++ = HEXL[(cp >> 4) & 15]; *w++ = HEXL[cp & 15];
      i += 3;
    } else {
      JE_PUTN(s.s + i, k);
      i += k;
    }
  }
  JE_PUTC('"');
  out->len = (size_t)(w - out->buf);
  out->buf[out->len] = 0;
#undef JE_NEED
#undef JE_PUTC
#undef JE_PUTN
}
static void json_stringify_val(StrBuf *out, JVal *v);
static void json_stringify_val(StrBuf *out, JVal *v) {
  switch (v->t) {
    case J_NULL: sb_put(out, "null"); break;
    case J_BOOL: sb_put(out, v->b ? "true" : "false"); break;
    case J_NUM: { StrBuf tmp; sb_init(&tmp); fmt_js_number(v->num, &tmp); sb_putn(out, tmp.buf, tmp.len); sb_free(&tmp); break; }
    case J_STR: json_escape_str(out, v->str); break;
    case J_ARR:
      sb_putc(out, '[');
      for (size_t i = 0; i < v->nitems; i++) {
        if (i) sb_putc(out, ',');
        json_stringify_val(out, v->items[i]);
      }
      sb_putc(out, ']');
      break;
    case J_OBJ:
      sb_putc(out, '{');
      for (size_t i = 0; i < v->n; i++) {
        if (i) sb_putc(out, ',');
        json_escape_str(out, v->keys[i]);
        sb_putc(out, ':');
        json_stringify_val(out, v->vals[i]);
      }
      sb_putc(out, '}');
      break;
  }
}

/* ------------------------------------------------------------ slug / words */
static const char *slug_decomp_lookup(unsigned cp) {
  size_t lo = 0, hi = SLUG_DECOMP_N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (SLUG_DECOMP[mid].cp < cp) lo = mid + 1;
    else if (SLUG_DECOMP[mid].cp > cp) hi = mid;
    else return SLUG_DECOMP[mid].tok;
  }
  return NULL;
}
static int is_mark_cp(unsigned cp) {
  size_t lo = 0, hi = MARK_RANGES_N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (MARK_RANGES[mid].hi < cp) lo = mid + 1;
    else if (MARK_RANGES[mid].lo > cp) hi = mid;
    else return 1;
  }
  return 0;
}
/* slugifySlow over code points; out is ASCII-only. */
static Str slugify_slow(Arena *a, Str text) {
  size_t cap = 128, len = 0;
  char *out = malloc(cap);
  int gap = 0;
#define SLUG_PUSH(ch) do { \
    if (gap) { if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); } out[len++] = '-'; } \
    gap = 0; \
    if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); } \
    out[len++] = (char)(ch); } while (0)
#define SLUG_SEP() do { gap = len > 0; } while (0)
  size_t i = 0;
  while (i < text.len) {
    unsigned cp; size_t k = utf8_decode(text.s, text.len, i, &cp);
    i += k;
    if (cp < 128) {
      if (cp >= 'A' && cp <= 'Z') SLUG_PUSH(cp + 32);
      else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) SLUG_PUSH(cp);
      else SLUG_SEP();
      continue;
    }
    if (is_mark_cp(cp)) continue;
    const char *tok = slug_decomp_lookup(cp);
    if (!tok) { SLUG_SEP(); continue; }
    for (const char *q = tok; *q; q++) {
      if (*q == 1) SLUG_SEP();
      else SLUG_PUSH(*q);
    }
  }
  if (len > 64) len = 64;
  while (len > 0 && out[len - 1] == '-') len--;
  Str r;
  if (len == 0) r = arena_str_c(a, "untitled");
  else r = arena_str(a, out, len);
  free(out);
  return r;
#undef SLUG_PUSH
#undef SLUG_SEP
}

typedef struct { Str in; Str out; int used; } SlugCacheEnt;
typedef struct { SlugCacheEnt *e; size_t n, cap; } SlugCache;
static unsigned long long slug_hash(Str s) {
  unsigned long long h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.len; i++) { h ^= (unsigned char)s.s[i]; h *= 1099511628211ULL; }
  return h;
}
static Str slug_cache_get(SlugCache *cache, Str text, int *found) {
  if (!cache->e) { *found = 0; return str_c(""); }
  size_t mask = cache->cap - 1;
  size_t idx = (size_t)slug_hash(text) & mask;
  while (cache->e[idx].used) {
    if (str_eq(cache->e[idx].in, text)) { *found = 1; return cache->e[idx].out; }
    idx = (idx + 1) & mask;
  }
  *found = 0;
  return str_c("");
}
static void slug_cache_put(SlugCache *cache, Str text, Str out) {
  if (!cache->e || (cache->n + 1) * 10 > cache->cap * 7) {
    size_t ncap = cache->cap ? cache->cap * 2 : 8192;
    SlugCacheEnt *ne = calloc(ncap, sizeof(SlugCacheEnt));
    if (!ne) exit(1);
    for (size_t i = 0; i < cache->cap; i++) {
      if (!cache->e[i].used) continue;
      size_t idx = (size_t)slug_hash(cache->e[i].in) & (ncap - 1);
      while (ne[idx].used) idx = (idx + 1) & (ncap - 1);
      ne[idx] = cache->e[i];
    }
    free(cache->e);
    cache->e = ne;
    cache->cap = ncap;
  }
  size_t mask = cache->cap - 1;
  size_t idx = (size_t)slug_hash(text) & mask;
  while (cache->e[idx].used) idx = (idx + 1) & mask;
  cache->e[idx].in = text;
  cache->e[idx].out = out;
  cache->e[idx].used = 1;
  cache->n++;
}
static Str slugify(Arena *a, SlugCache *cache, Str text) {
  int found = 0;
  Str hit = slug_cache_get(cache, text, &found);
  if (found) return hit;
  /* ASCII fast path */
  int ascii = 1;
  for (size_t i = 0; i < text.len; i++) if ((unsigned char)text.s[i] > 127) { ascii = 0; break; }
  Str r;
  if (!ascii) {
    r = slugify_slow(a, text);
  } else {
    char *out = malloc(text.len + 1);
    size_t len = 0; int gap = 0;
    for (size_t i = 0; i < text.len; i++) {
      unsigned char ch = (unsigned char)text.s[i];
      int keep = 0;
      if (ch >= 'A' && ch <= 'Z') { keep = ch + 32; }
      else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
      if (!keep) { gap = len > 0; continue; }
      if (gap) out[len++] = '-';
      gap = 0;
      out[len++] = (char)keep;
    }
    if (len > 64) len = 64;
    while (len > 0 && out[len - 1] == '-') len--;
    r = len == 0 ? arena_str_c(a, "untitled") : arena_str(a, out, len);
    free(out);
  }
  slug_cache_put(cache, text, r);
  return r;
}

typedef struct { Str *items; size_t n, cap; } StrSet;
static int strset_has(StrSet *s, Str v) {
  for (size_t i = 0; i < s->n; i++) if (str_eq(s->items[i], v)) return 1;
  return 0;
}
static void strset_add(StrSet *s, Str v) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 16;
    s->items = realloc(s->items, s->cap * sizeof(Str));
    if (!s->items) exit(1);
  }
  s->items[s->n++] = v;
}
/* uniqueSlug: first of slug, slug-2, slug-3... not in taken; adds it. */
static Str unique_slug(Arena *a, Str slug, StrSet *taken) {
  if (!strset_has(taken, slug)) { strset_add(taken, slug); return slug; }
  for (int n = 2;; n++) {
    char num[24];
    int nl = snprintf(num, sizeof num, "-%d", n);
    Str cand = arena_str_cat3(a, slug, str_make(num, (size_t)nl), str_make("", 0));
    if (!strset_has(taken, cand)) { strset_add(taken, cand); return cand; }
  }
}

static int words_simple(Str text) {
  int n = 0, inW = 0;
  for (size_t i = 0; i < text.len; i++) {
    unsigned char c = (unsigned char)text.s[i];
    if (c <= 32) inW = 0;
    else if (!inW) { inW = 1; n++; }
  }
  return n;
}
static int words_full(Str text) {
  int n = 0, inWord = 0;
  size_t i = 0;
  while (i < text.len) {
    unsigned cp; size_t k = utf8_decode(text.s, text.len, i, &cp);
    i += k;
    if (is_ws_code(cp)) inWord = 0;
    else if (!inWord) { inWord = 1; n++; }
  }
  return n;
}
static int words_of(Str text) {
  size_t i = 0;
  while (i < text.len) {
    unsigned cp; size_t k = utf8_decode(text.s, text.len, i, &cp);
    i += k;
    if (is_uni_ws(cp)) return words_full(text);
  }
  return words_simple(text);
}

static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"};
static Str long_date(Arena *a, Str date) {
  /* date is YYYY-MM-DD (validated). "9 November 2026" */
  int day = (date.s[8] - '0') * 10 + (date.s[9] - '0');
  int mo = (date.s[5] - '0') * 10 + (date.s[6] - '0');
  char tmp[64];
  int n = snprintf(tmp, sizeof tmp, "%d %s %c%c%c%c", day, MONTHS[mo - 1],
                   date.s[0], date.s[1], date.s[2], date.s[3]);
  return arena_str(a, tmp, (size_t)n);
}

/* ---------------------------------------------------------- front matter */
static const char *FM_KEYS[] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};
typedef struct {
  int has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
  Str title, date, layout, slug, summary;
  int draft;
  Str *tags; size_t ntags;
} FMData;

static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}
static int is_digit_b(char c) { return c >= '0' && c <= '9'; }

/* stringOf: returns 1 ok (value in *out) or 0 with error in *err. */
static int fm_string_of(Arena *a, Str raw, Str *out, Str *err) {
  if (raw.len > 0 && raw.s[0] == '"') {
    if (raw.len < 2 || raw.s[raw.len - 1] != '"') {
      *err = str_c("a quoted string is not closed"); return 0;
    }
    StrBuf b; sb_init(&b);
    for (size_t i = 1; i + 1 < raw.len; i++) {
      char c = raw.s[i];
      if (c == '\\') {
        char n = raw.s[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.len - 1) {
          sb_free(&b);
          *err = str_c("a quoted string holds an escape other than \\\" or \\\\");
          return 0;
        }
        sb_putc(&b, n);
        i++;
      } else if (c == '"') {
        sb_free(&b);
        *err = str_c("a quoted string holds a stray quote");
        return 0;
      } else sb_putc(&b, c);
    }
    *out = sb_to_arena(a, &b);
    return 1;
  }
  if (raw.len > 0 && raw.s[0] == '[') {
    *err = str_c("a bare string opens on [");
    return 0;
  }
  *out = raw;
  return 1;
}

/* valueOf: 1 ok, 0 with error text (without the key prefix for most). */
static int fm_value_of(Arena *a, const char *key, Str raw, FMData *d, Str *err) {
  if (!strcmp(key, "title") || !strcmp(key, "summary")) {
    Str v;
    if (!fm_string_of(a, raw, &v, err)) {
      /* wrap: key "title": <err> -- build in caller via errbuf */
      char tmp[512];
      int n = snprintf(tmp, sizeof tmp, "key \"%s\": %.*s", key, (int)err->len, err->s);
      *err = arena_str(a, tmp, (size_t)n);
      return 0;
    }
    if (!strcmp(key, "title")) { d->title = v; d->has_title = 1; }
    else { d->summary = v; d->has_summary = 1; }
    return 1;
  }
  if (!strcmp(key, "date")) {
    int ok = raw.len == 10 && raw.s[4] == '-' && raw.s[7] == '-';
    if (ok) for (int i = 0; i < 10; i++)
      if (i != 4 && i != 7 && !is_digit_b(raw.s[i])) { ok = 0; break; }
    int y = 0, mo = 0, dd = 0;
    if (ok) {
      y = (raw.s[0]-'0')*1000 + (raw.s[1]-'0')*100 + (raw.s[2]-'0')*10 + (raw.s[3]-'0');
      mo = (raw.s[5]-'0')*10 + (raw.s[6]-'0');
      dd = (raw.s[8]-'0')*10 + (raw.s[9]-'0');
    }
    if (!ok || y < 1970 || y > 2099 || mo < 1 || mo > 12 || dd < 1 || dd > days_in(y, mo)) {
      *err = str_c("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return 0;
    }
    d->date = raw; d->has_date = 1;
    return 1;
  }
  if (!strcmp(key, "tags")) {
    if (raw.len == 0 || raw.s[0] != '[' || raw.s[raw.len - 1] != ']') {
      *err = str_c("key \"tags\": not a list [a, b]");
      return 0;
    }
    Str inner = str_make(raw.s + 1, raw.len - 2);
    size_t ta, tb;
    trim_js_range(inner.s, 0, inner.len, &ta, &tb);
    if (ta == tb) { d->tags = NULL; d->ntags = 0; d->has_tags = 1; return 1; }
    /* split inner on ',' */
    size_t cap = 4, n = 0;
    Str *tags = malloc(cap * sizeof(Str));
    size_t start = 0;
    for (size_t i = 0; i <= inner.len; i++) {
      if (i == inner.len || inner.s[i] == ',') {
        Str piece = str_make(inner.s + start, i - start);
        size_t pa, pb;
        trim_js_range(piece.s, 0, piece.len, &pa, &pb);
        Str t = str_make(piece.s + pa, pb - pa);
        if (t.len == 0) { free(tags); *err = str_c("key \"tags\": an empty tag, or one holding \" [ or ]"); return 0; }
        for (size_t j = 0; j < t.len; j++) {
          char c = t.s[j];
          if (c == '"' || c == '[' || c == ']') {
            free(tags); *err = str_c("key \"tags\": an empty tag, or one holding \" [ or ]"); return 0;
          }
        }
        if (n == cap) { cap *= 2; tags = realloc(tags, cap * sizeof(Str)); }
        tags[n++] = t;
        start = i + 1;
      }
    }
    /* move into arena */
    Str *atags = arena_alloc(a, (n ? n : 1) * sizeof(Str));
    for (size_t i = 0; i < n; i++) atags[i] = tags[i];
    free(tags);
    d->tags = atags; d->ntags = n; d->has_tags = 1;
    return 1;
  }
  if (!strcmp(key, "draft")) {
    if (str_eq_c(raw, "true")) { d->draft = 1; d->has_draft = 1; return 1; }
    if (str_eq_c(raw, "false")) { d->draft = 0; d->has_draft = 1; return 1; }
    *err = str_c("key \"draft\": true or false");
    return 0;
  }
  if (!strcmp(key, "layout")) {
    if (raw.len == 0) { *err = str_c("key \"layout\": not a template name"); return 0; }
    for (size_t i = 0; i < raw.len; i++) {
      char c = raw.s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
        *err = str_c("key \"layout\": not a template name"); return 0;
      }
    }
    d->layout = raw; d->has_layout = 1;
    return 1;
  }
  if (!strcmp(key, "slug")) {
    if (raw.len == 0 || raw.len > 64) { *err = str_c("key \"slug\": not a slug"); return 0; }
    int prevDash = 1;
    for (size_t i = 0; i < raw.len; i++) {
      char c = raw.s[i];
      if (c == '-') { if (prevDash) { *err = str_c("key \"slug\": not a slug"); return 0; } prevDash = 1; }
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) prevDash = 0;
      else { *err = str_c("key \"slug\": not a slug"); return 0; }
    }
    if (prevDash) { *err = str_c("key \"slug\": not a slug"); return 0; }
    d->slug = raw; d->has_slug = 1;
    return 1;
  }
  {
    char tmp[128];
    int n = snprintf(tmp, sizeof tmp, "unknown key \"%s\"", key);
    *err = arena_str(a, tmp, (size_t)n);
    return 0;
  }
}

/* parseFrontMatter: 1 ok (data in *d, body in *body), 0 with error in *err. */
static int parse_front_matter(Arena *a, Str text, FMData *d, Str *body, Str *err) {
  memset(d, 0, sizeof *d);
  if (!(text.len == 3 && memcmp(text.s, "---", 3) == 0) &&
      !(text.len >= 4 && memcmp(text.s, "---\n", 4) == 0)) {
    *err = str_c("no front matter");
    return 0;
  }
  if (text.len == 3) { *err = str_c("the front matter is not closed"); return 0; }
  /* find closing line */
  size_t pos = 4;
  long close_start = -1;
  while (pos < text.len) {
    const char *nlp = memchr(text.s + pos, '\n', text.len - pos);
    size_t line_end = nlp ? (size_t)(nlp - text.s) : text.len;
    if (line_end - pos == 3 && text.s[pos] == '-' && text.s[pos + 1] == '-' && text.s[pos + 2] == '-') {
      close_start = (long)pos;
      break;
    }
    pos = line_end + 1;
    if (!nlp) break;
  }
  if (close_start < 0) { *err = str_c("the front matter is not closed"); return 0; }
  size_t p = 4;
  long idx = 1;
  while (p < (size_t)close_start) {
    const char *nlp = memchr(text.s + p, '\n', text.len - p);
    size_t line_end = nlp ? (size_t)(nlp - text.s) : text.len;
    Str line = str_make(text.s + p, line_end - p);
    int blank = 1;
    for (size_t i = 0; i < line.len; i++) {
      char c = line.s[i];
      if (c != ' ' && c != '\t') { blank = 0; break; }
    }
    if (!blank && line.len > 0 && line.s[0] != '#') {
      size_t k = 0;
      while (k < line.len) {
        char c = line.s[k];
        if ((c >= 'a' && c <= 'z') || c == '_') k++;
        else break;
      }
      if (k == 0 || k >= line.len || line.s[k] != ':') {
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "line %ld is not \"key: value\"", idx + 1);
        *err = arena_str(a, tmp, (size_t)n);
        return 0;
      }
      Str key = str_make(line.s, k);
      Str raw;
      if (k + 1 == line.len) raw = str_make("", 0);
      else if (line.s[k + 1] == ' ') raw = str_make(line.s + k + 2, line.len - (k + 2));
      else {
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "line %ld is not \"key: value\"", idx + 1);
        *err = arena_str(a, tmp, (size_t)n);
        return 0;
      }
      size_t e = raw.len;
      while (e > 0 && (raw.s[e - 1] == ' ' || raw.s[e - 1] == '\t')) e--;
      raw.len = e;
      /* key must be known */
      const char *keyname = NULL;
      for (size_t ki = 0; ki < sizeof(FM_KEYS) / sizeof(char *); ki++)
        if (str_eq_c(key, FM_KEYS[ki])) { keyname = FM_KEYS[ki]; break; }
      if (!keyname) {
        char tmp[128];
        int n = snprintf(tmp, sizeof tmp, "unknown key \"%.*s\"", (int)key.len, key.s);
        *err = arena_str(a, tmp, (size_t)n);
        return 0;
      }
      int seen = 0;
      if (!strcmp(keyname, "title")) seen = d->has_title;
      else if (!strcmp(keyname, "date")) seen = d->has_date;
      else if (!strcmp(keyname, "tags")) seen = d->has_tags;
      else if (!strcmp(keyname, "draft")) seen = d->has_draft;
      else if (!strcmp(keyname, "layout")) seen = d->has_layout;
      else if (!strcmp(keyname, "slug")) seen = d->has_slug;
      else if (!strcmp(keyname, "summary")) seen = d->has_summary;
      if (seen) {
        char tmp[128];
        int n = snprintf(tmp, sizeof tmp, "key \"%s\" is given twice", keyname);
        *err = arena_str(a, tmp, (size_t)n);
        return 0;
      }
      if (raw.len == 0) {
        char tmp[128];
        int n = snprintf(tmp, sizeof tmp, "key \"%s\" has no value", keyname);
        *err = arena_str(a, tmp, (size_t)n);
        return 0;
      }
      Str verr;
      if (!fm_value_of(a, keyname, raw, d, &verr)) { *err = verr; return 0; }
    }
    p = line_end + 1;
    idx++;
  }
  if (!d->has_title) { *err = str_c("no title"); return 0; }
  const char *nlp = memchr(text.s + close_start, '\n', text.len - (size_t)close_start);
  if (!nlp) *body = str_make("", 0);
  else {
    size_t bs = (size_t)(nlp - text.s) + 1;
    *body = str_make(text.s + bs, text.len - bs);
  }
  return 1;
}

/* --------------------------------------------------------------- markdown */
static void esc_html_range(StrBuf *out, const char *s, size_t a, size_t b) {
  size_t seg = a;
  for (size_t i = a; i < b; i++) {
    const char *rep = NULL;
    switch (s[i]) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = "&#39;"; break;
    }
    if (rep) {
      if (i > seg) sb_putn(out, s + seg, i - seg);
      sb_put(out, rep);
      seg = i + 1;
    }
  }
  if (b > seg) sb_putn(out, s + seg, b - seg);
}

typedef struct MdEnv MdEnv;
struct MdEnv {
  Str (*link)(void *ctx, Str target);
  void *ctx;
};

static int is_escapable_b(char c) {
  const char *E = "\\`*_[]()#+-.!>";
  return strchr(E, c) != NULL;
}
static int is_escaped_at(const char *s, size_t p) {
  size_t k = 0;
  while (p > 0 && s[p - 1] == '\\') { k++; p--; }
  return (k % 2) == 1;
}
/* find `what` in s[from,to) at an unescaped position; -1 if none. */
static long find_unescaped(const char *s, const char *what, size_t from, size_t to) {
  size_t wl = strlen(what);
  if (wl == 0) return -1;
  for (size_t j = from; j + wl <= to; j++) {
    if (memcmp(s + j, what, wl) == 0) {
      if (!is_escaped_at(s, j)) return (long)j;
      /* keep scanning from j+1: emulate indexOf loop */
    }
  }
  return -1;
}
static long find_single_star(const char *s, size_t from, size_t to) {
  for (size_t j = from; j < to; j++) {
    if (s[j] != '*') continue;
    if (is_escaped_at(s, j)) continue;
    if (j + 1 < to && s[j + 1] == '*') { j++; continue; }
    return (long)j;
  }
  return -1;
}
static void inline_into(StrBuf *out, const char *s, size_t len, size_t lo, size_t hi, MdEnv *env) {
  size_t i = lo, seg = lo;
  while (i < hi) {
    /* find next special byte among \ ` * [ */
    size_t j = i;
    while (j < hi && s[j] != '\\' && s[j] != '`' && s[j] != '*' && s[j] != '[') j++;
    if (j >= hi) break;
    char ch = s[j];
    if (ch == '\\') {
      if (j + 1 < hi && is_escapable_b(s[j + 1])) {
        if (j > seg) esc_html_range(out, s, seg, j);
        esc_html_range(out, s, j + 1, j + 2);
        i = j + 2; seg = i;
      } else i = j + 1;
      continue;
    }
    if (ch == '`') {
      long k = -1;
      for (size_t q = j + 1; q < hi; q++) if (s[q] == '`') { k = (long)q; break; }
      if (k != -1 && k > (long)j + 1) {
        if (j > seg) esc_html_range(out, s, seg, j);
        sb_put(out, "<code>");
        esc_html_range(out, s, j + 1, (size_t)k);
        sb_put(out, "</code>");
        i = (size_t)k + 1; seg = i;
      } else i = j + 1;
      continue;
    }
    if (ch == '*') {
      if (j + 1 < hi && s[j + 1] == '*') {
        long k = find_unescaped(s, "**", j + 2, hi);
        if (k > (long)j + 2) {
          if (j > seg) esc_html_range(out, s, seg, j);
          sb_put(out, "<strong>");
          inline_into(out, s, len, j + 2, (size_t)k, env);
          sb_put(out, "</strong>");
          i = (size_t)k + 2; seg = i;
        } else i = j + 2;
        continue;
      }
      long k = find_single_star(s, j + 1, hi);
      if (k > (long)j + 1) {
        if (j > seg) esc_html_range(out, s, seg, j);
        sb_put(out, "<em>");
        inline_into(out, s, len, j + 1, (size_t)k, env);
        sb_put(out, "</em>");
        i = (size_t)k + 1; seg = i;
      } else i = j + 1;
      continue;
    }
    /* '[' */
    long k = find_unescaped(s, "]", j + 1, hi);
    long m = -1;
    if (k != -1 && (size_t)k + 1 < hi && s[k + 1] == '(') {
      for (size_t q = (size_t)k + 2; q < hi; q++) if (s[q] == ')') { m = (long)q; break; }
    }
    Str target = str_make("", 0);
    if (m != -1) target = str_make(s + k + 2, (size_t)(m - (k + 2)));
    if (target.len > 0 && !has_js_ws(target)) {
      if (j > seg) esc_html_range(out, s, seg, j);
      Str href = env->link(env->ctx, target);
      sb_put(out, "<a href=\"");
      esc_html_range(out, href.s, 0, href.len);
      sb_put(out, "\">");
      inline_into(out, s, len, j + 1, (size_t)k, env);
      sb_put(out, "</a>");
      i = (size_t)m + 1; seg = i;
    } else i = j + 1;
  }
  if (seg < hi) esc_html_range(out, s, seg, hi);
}

typedef struct { int level; Str id; Str html; } TocItem;
typedef struct { size_t a, b; } Range;
typedef struct {
  Arena *arena; Str src; size_t *starts; size_t n; StrBuf *out; MdEnv *env;
  TocItem *toc; size_t ntoc, captoc;
  Str excerpt; int has_excerpt;
  SlugCache *slugcache; StrSet ids;
} MdState;
static size_t line_b(MdState *m, size_t k) { return k + 1 < m->n ? m->starts[k + 1] - 1 : m->src.len; }
static int is_space_tab_b(char c) { return c == ' ' || c == '\t'; }
static int is_blank_r(MdState *m, size_t a, size_t b) {
  for (size_t i = a; i < b; i++) { char c = m->src.s[i]; if (c != ' ' && c != '\t') return 0; }
  return 1;
}
static void trim_slice(const char *s, size_t a, size_t b, size_t *ra, size_t *rb) {
  while (a < b && is_space_tab_b(s[a])) a++;
  while (b > a && is_space_tab_b(s[b - 1])) b--;
  *ra = a; *rb = b;
}
/* fence open: returns 1 and lang in *lang (may be empty). */
static int fence_lang_r(MdState *m, size_t a, size_t b, Str *lang) {
  const char *s = m->src.s;
  if (b - a < 3 || s[a] != '`' || s[a + 1] != '`' || s[a + 2] != '`') return 0;
  size_t j = a + 3;
  while (j < b) {
    char c = s[j];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '+' || c == '-') j++;
    else break;
  }
  *lang = str_make(s + a + 3, j - (a + 3));
  while (j < b && is_space_tab_b(s[j])) j++;
  return j == b;
}
static int is_fence_close_r(MdState *m, size_t a, size_t b) {
  const char *s = m->src.s;
  if (b - a < 3 || s[a] != '`' || s[a + 1] != '`' || s[a + 2] != '`') return 0;
  for (size_t j = a + 3; j < b; j++) if (!is_space_tab_b(s[j])) return 0;
  return 1;
}
static int heading_of_r(MdState *m, size_t a, size_t b, int *depth, size_t *ta, size_t *tb) {
  const char *s = m->src.s;
  size_t n = 0;
  while (a + n < b && s[a + n] == '#') n++;
  if (n == 0 || n > 6) return 0;
  if (a + n >= b || !is_space_tab_b(s[a + n])) return 0;
  size_t j = a + n;
  while (j < b && is_space_tab_b(s[j])) j++;
  size_t e = b;
  while (e > j && is_space_tab_b(s[e - 1])) e--;
  if (e == j) return 0;
  *depth = (int)n; *ta = j; *tb = e;
  return 1;
}
static int is_hr_r(MdState *m, size_t a, size_t b) {
  const char *s = m->src.s;
  if (b - a < 3) return 0;
  char c = s[a];
  if (c != '-' && c != '*') return 0;
  if (s[a + 1] != c || s[a + 2] != c) return 0;
  for (size_t j = a + 3; j < b; j++) if (!is_space_tab_b(s[j])) return 0;
  return 1;
}
static int is_continued_r(MdState *m, size_t a, size_t b) {
  const char *s = m->src.s;
  size_t sp = 0;
  while (a + sp < b && s[a + sp] == ' ') sp++;
  if (sp < 2 || a + sp >= b) return 0;
  unsigned cp;
  utf8_decode(s, b, a + sp, &cp);
  return !is_ws_code(cp);
}
static void inline_text(MdState *m, Range *ranges, size_t nr) {
  if (nr == 1) {
    size_t a = ranges[0].a, b = ranges[0].b;
    while (a < b && is_space_tab_b(m->src.s[a])) a++;
    while (b > a && is_space_tab_b(m->src.s[b - 1])) b--;
    inline_into(m->out, m->src.s, m->src.len, a, b, m->env);
    return;
  }
  int needs = 0;
  for (size_t k = 0; k < nr; k++) {
    size_t a = ranges[k].a, b = ranges[k].b;
    if (a < b && (is_space_tab_b(m->src.s[a]) || is_space_tab_b(m->src.s[b - 1]))) { needs = 1; break; }
  }
  if (!needs) {
    inline_into(m->out, m->src.s, m->src.len, ranges[0].a, ranges[nr - 1].b, m->env);
  } else {
    StrBuf tmp; sb_init(&tmp);
    for (size_t k = 0; k < nr; k++) {
      size_t a, b;
      trim_slice(m->src.s, ranges[k].a, ranges[k].b, &a, &b);
      if (k) sb_putc(&tmp, '\n');
      sb_putn(&tmp, m->src.s + a, b - a);
    }
    inline_into(m->out, tmp.buf ? tmp.buf : "", tmp.len, 0, tmp.len, m->env);
    sb_free(&tmp);
  }
}
static void do_list(MdState *m, int is_ordered, long start, size_t first_a, size_t first_b, size_t *ip) {
  /* items: ranges stored contiguously; item k spans
   * ranges[item_off[k] .. item_off[k]+item_cnt[k]). */
  Range *ranges = NULL;
  size_t nranges = 0, capranges = 0;
  size_t *item_off = NULL, *item_cnt = NULL, nitems = 0, capitems = 0;
#define LIST_PUSH_RANGE(aa, bb) do { \
    if (nranges == capranges) { capranges = capranges ? capranges * 2 : 16; \
      ranges = realloc(ranges, capranges * sizeof(Range)); if (!ranges) exit(1); } \
    ranges[nranges].a = (aa); ranges[nranges].b = (bb); nranges++; } while (0)
#define LIST_NEW_ITEM(aa, bb) do { \
    if (nitems == capitems) { capitems = capitems ? capitems * 2 : 4; \
      item_off = realloc(item_off, capitems * sizeof(size_t)); \
      item_cnt = realloc(item_cnt, capitems * sizeof(size_t)); \
      if (!item_off || !item_cnt) exit(1); } \
    item_off[nitems] = nranges; item_cnt[nitems] = 0; nitems++; \
    LIST_PUSH_RANGE(aa, bb); item_cnt[nitems - 1]++; } while (0)
  LIST_NEW_ITEM(first_a, first_b);
  size_t i = *ip + 1;
  while (i < m->n) {
    size_t a = m->starts[i], b = line_b(m, i);
    long mA = -1; size_t mB = 0;
    if (is_ordered) {
      size_t j = a;
      while (j < b && m->src.s[j] >= '0' && m->src.s[j] <= '9') j++;
      if (j - a >= 1 && j - a <= 9 && j < b && m->src.s[j] == '.' && j + 1 < b && m->src.s[j + 1] == ' ') {
        mA = (long)(j + 2); mB = b;
      }
    } else if ((m->src.s[a] == '-' || m->src.s[a] == '*') && a + 1 < b && m->src.s[a + 1] == ' ') {
      mA = (long)(a + 2); mB = b;
    }
    if (mA != -1) {
      LIST_NEW_ITEM((size_t)mA, mB);
      i++;
    } else if (is_continued_r(m, a, b)) {
      LIST_PUSH_RANGE(a, b);
      item_cnt[nitems - 1]++;
      i++;
    } else break;
  }
  *ip = i;
  const char *tag = is_ordered ? "ol" : "ul";
  if (is_ordered && start != 1) {
    char tmp[48];
    int n = snprintf(tmp, sizeof tmp, "<ol start=\"%ld\">\n", start);
    sb_putn(m->out, tmp, (size_t)n);
  } else {
    sb_putc(m->out, '<'); sb_put(m->out, tag); sb_put(m->out, ">\n");
  }
  for (size_t k = 0; k < nitems; k++) {
    sb_put(m->out, "<li>");
    inline_text(m, ranges + item_off[k], item_cnt[k]);
    sb_put(m->out, "</li>\n");
  }
  sb_put(m->out, "</");
  sb_put(m->out, tag);
  sb_put(m->out, ">\n");
  free(ranges);
  free(item_off);
  free(item_cnt);
#undef LIST_PUSH_RANGE
#undef LIST_NEW_ITEM
}

typedef struct { Str html; TocItem *toc; size_t ntoc; Str excerpt; } MdResult;
static MdResult render_markdown(Arena *a, Str src, MdEnv *env, SlugCache *slugcache) {
  MdState m;
  memset(&m, 0, sizeof m);
  m.arena = a; m.src = src; m.env = env; m.slugcache = slugcache;
  /* line starts */
  size_t cap = 64, n = 0;
  size_t *starts = malloc(cap * sizeof(size_t));
  starts[n++] = 0;
  size_t p = 0;
  for (;;) {
    const char *nlp = memchr(src.s + p, '\n', src.len - p);
    if (!nlp) break;
    if (n == cap) { cap *= 2; starts = realloc(starts, cap * sizeof(size_t)); }
    starts[n++] = (size_t)(nlp - src.s) + 1;
    p = (size_t)(nlp - src.s) + 1;
  }
  m.starts = starts; m.n = n;
  StrBuf out; sb_init(&out);
  m.out = &out;
  size_t i = 0;
  while (i < m.n) {
    size_t la = m.starts[i], lb = line_b(&m, i);
    if (la == lb) { i++; continue; }
    unsigned char c0 = (unsigned char)src.s[la];
    if (c0 == ' ' || c0 == '\t') {
      if (is_blank_r(&m, la, lb)) { i++; continue; }
      /* indented non-blank: paragraph */
    } else if (c0 == '`') {
      Str lang;
      if (fence_lang_r(&m, la, lb, &lang)) {
        sb_put(&out, "<pre><code");
        if (lang.len > 0) {
          sb_put(&out, " class=\"language-");
          esc_html_range(&out, lang.s, 0, lang.len);
          sb_putc(&out, '"');
        }
        sb_putc(&out, '>');
        i++;
        size_t fs = i;
        while (i < m.n) {
          if (is_fence_close_r(&m, m.starts[i], line_b(&m, i))) break;
          i++;
        }
        if (i > fs) {
          size_t end = i < m.n ? m.starts[i] - 1 : src.len;
          esc_html_range(&out, src.s, m.starts[fs], end);
          sb_putc(&out, '\n');
        }
        i++;
        sb_put(&out, "</code></pre>\n");
        continue;
      }
    } else if (c0 == '#') {
      int depth; size_t ta, tb;
      if (heading_of_r(&m, la, lb, &depth, &ta, &tb)) {
        Str htext = str_make(src.s + ta, tb - ta);
        Str id = unique_slug(a, slugify(a, slugcache, htext), &m.ids);
        char tmp[32];
        int tn = snprintf(tmp, sizeof tmp, "<h%d id=\"", depth);
        sb_putn(&out, tmp, (size_t)tn);
        sb_puts(&out, id);
        sb_put(&out, "\">");
        size_t mark = out.len;
        inline_into(&out, src.s, src.len, ta, tb, env);
        Str hhtml = arena_str(a, out.buf + mark, out.len - mark);
        tn = snprintf(tmp, sizeof tmp, "</h%d>\n", depth);
        sb_putn(&out, tmp, (size_t)tn);
        if (depth == 2 || depth == 3) {
          if (m.ntoc == m.captoc) {
            m.captoc = m.captoc ? m.captoc * 2 : 8;
            m.toc = realloc(m.toc, m.captoc * sizeof(TocItem));
          }
          m.toc[m.ntoc].level = depth;
          m.toc[m.ntoc].id = id;
          m.toc[m.ntoc].html = hhtml;
          m.ntoc++;
        }
        i++;
        continue;
      }
    } else if (c0 == '-' || c0 == '*') {
      if (is_hr_r(&m, la, lb)) { sb_put(&out, "<hr>\n"); i++; continue; }
      if (la + 1 < lb && src.s[la + 1] == ' ') { do_list(&m, 0, 1, la + 2, lb, &i); continue; }
    } else if (c0 == '>') {
      Range *body = malloc(sizeof(Range) * 8);
      size_t nb = 0, capb = 8;
      while (i < m.n) {
        size_t ba = m.starts[i], bb = line_b(&m, i);
        if (src.s[ba] != '>') break;
        if (nb == capb) { capb *= 2; body = realloc(body, capb * sizeof(Range)); }
        body[nb].a = (ba + 1 < bb && src.s[ba + 1] == ' ') ? ba + 2 : ba + 1;
        body[nb].b = bb;
        nb++;
        i++;
      }
      /* kept = trimmed non-empty */
      StrBuf joined; sb_init(&joined);
      size_t nkept = 0;
      for (size_t k = 0; k < nb; k++) {
        size_t ta, tb;
        trim_slice(src.s, body[k].a, body[k].b, &ta, &tb);
        if (ta == tb) continue;
        if (nkept) sb_putc(&joined, '\n');
        sb_putn(&joined, src.s + ta, tb - ta);
        nkept++;
      }
      free(body);
      if (nkept == 0) sb_put(&out, "<blockquote>\n</blockquote>\n");
      else {
        sb_put(&out, "<blockquote>\n<p>");
        inline_into(&out, joined.buf ? joined.buf : "", joined.len, 0, joined.len, env);
        sb_put(&out, "</p>\n</blockquote>\n");
      }
      sb_free(&joined);
      continue;
    } else if (c0 >= '0' && c0 <= '9') {
      size_t j = la;
      while (j < lb && src.s[j] >= '0' && src.s[j] <= '9') j++;
      if (j - la >= 1 && j - la <= 9 && j < lb && src.s[j] == '.' && j + 1 < lb && src.s[j + 1] == ' ') {
        long start = 0;
        for (size_t q = la; q < j; q++) start = start * 10 + (src.s[q] - '0');
        do_list(&m, 1, start, j + 2, lb, &i);
        continue;
      }
    }
    /* paragraph */
    Range *body = malloc(sizeof(Range) * 8);
    size_t nb = 0, capb = 8;
    body[nb].a = la; body[nb].b = lb; nb++;
    i++;
    while (i < m.n) {
      size_t ba = m.starts[i], bb = line_b(&m, i);
      if (ba == bb) break;
      unsigned char c1 = (unsigned char)src.s[ba];
      if (c1 == ' ' || c1 == '\t') { if (is_blank_r(&m, ba, bb)) break; }
      else if (c1 == '`') { Str lg; if (fence_lang_r(&m, ba, bb, &lg)) break; }
      else if (c1 == '#') { int d2; size_t t2a, t2b; if (heading_of_r(&m, ba, bb, &d2, &t2a, &t2b)) break; }
      else if (c1 == '-' || c1 == '*') { if (is_hr_r(&m, ba, bb)) break; }
      if (nb == capb) { capb *= 2; body = realloc(body, capb * sizeof(Range)); }
      body[nb].a = ba; body[nb].b = bb; nb++;
      i++;
    }
    sb_put(&out, "<p>");
    size_t mark = out.len;
    inline_text(&m, body, nb);
    free(body);
    if (!m.has_excerpt) {
      m.excerpt = arena_str(a, out.buf + mark, out.len - mark);
      m.has_excerpt = 1;
    }
    sb_put(&out, "</p>\n");
  }
  MdResult r;
  r.html = sb_to_arena(a, &out);
  r.toc = m.toc; r.ntoc = m.ntoc;
  r.excerpt = m.has_excerpt ? m.excerpt : str_c("");
  free(starts);
  return r;
}

/* -------------------------------------------------------------- templates */
typedef struct Node Node;
typedef struct { Node **v; size_t n, cap; } NodeList;
typedef enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE } NKind;
struct Node {
  NKind kind;
  Str text;              /* N_TEXT */
  Str *path; int npath;  /* N_VAR/N_RAW/N_IF/N_FOR */
  int not_;              /* N_IF */
  Str varname;           /* N_FOR */
  Str inc_name;          /* N_INCLUDE */
  NodeList body;         /* N_FOR body, N_IF then */
  NodeList els; int has_else; /* N_IF */
};
static void nodelist_push(NodeList *l, Node *nd) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->v = realloc(l->v, l->cap * sizeof(Node *));
    if (!l->v) exit(1);
  }
  l->v[l->n++] = nd;
}
static Node *node_new(Arena *a, NKind k) {
  Node *nd = arena_alloc(a, sizeof(Node));
  memset(nd, 0, sizeof *nd);
  nd->kind = k;
  return nd;
}

typedef struct { int kind; Str text; Str *path; int npath; Str tag; } Tok;
enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG };
typedef struct { Tok *v; size_t n, cap; } TokList;
static void toklist_push(TokList *l, Tok t) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 16;
    l->v = realloc(l->v, l->cap * sizeof(Tok));
    if (!l->v) exit(1);
  }
  l->v[l->n++] = t;
}
static size_t find_substr(const char *s, size_t len, size_t from, const char *needle) {
  size_t nl = strlen(needle);
  if (nl == 0 || from + nl > len) return (size_t)-1;
  for (size_t i = from; i + nl <= len; i++)
    if (memcmp(s + i, needle, nl) == 0) return i;
  return (size_t)-1;
}
static int all_space_tab(const char *s, size_t a, size_t b) {
  for (size_t i = a; i < b; i++) if (s[i] != ' ' && s[i] != '\t') return 0;
  return 1;
}
static Str tmpl_err(Arena *a, Str name, const char *msg) {
  StrBuf b; sb_init(&b);
  sb_put(&b, "template \"");
  sb_puts(&b, name);
  sb_put(&b, "\": ");
  sb_put(&b, msg);
  return sb_to_arena(a, &b);
}
/* pathOf: validate PATH_RE on trimmed text; split into segments. */
static int path_of(Arena *a, Str name, Str text, Str **out_path, int *out_n, Str *err) {
  /* validate: seg(.seg)* where seg = [A-Za-z_][A-Za-z0-9_]* */
  if (text.len == 0) goto bad;
  {
    size_t i = 0;
    int nseg = 1;
    for (;;) {
      if (i >= text.len) goto bad;
      char c = text.s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) goto bad;
      i++;
      while (i < text.len) {
        c = text.s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') i++;
        else break;
      }
      if (i == text.len) break;
      if (text.s[i] != '.') goto bad;
      i++;
      nseg++;
    }
    /* split */
    Str *segs = arena_alloc(a, (size_t)nseg * sizeof(Str));
    size_t start = 0; int k = 0;
    for (size_t j = 0; j <= text.len; j++) {
      if (j == text.len || text.s[j] == '.') {
        segs[k++] = str_make(text.s + start, j - start);
        start = j + 1;
      }
    }
    *out_path = segs; *out_n = nseg;
    return 1;
  }
bad: {
    StrBuf b; sb_init(&b);
    sb_put(&b, "template \"");
    sb_puts(&b, name);
    sb_put(&b, "\": ");
    json_escape_str(&b, text);
    sb_put(&b, " is not a path");
    *err = sb_to_arena(a, &b);
    return 0;
  }
}

static int tokenize(Arena *a, Str name, Str src, TokList *out, Str *err) {
  memset(out, 0, sizeof *out);
  StrBuf text; sb_init(&text);
  size_t i = 0;
  while (i < src.len) {
    size_t open = i;
    while (open < src.len && src.s[open] != '{') open++;
    if (open == src.len || open == src.len - 1) { sb_putn(&text, src.s + i, src.len - i); break; }
    char next = src.s[open + 1];
    if (next != '{' && next != '%') { sb_putn(&text, src.s + i, open + 1 - i); i = open + 1; continue; }
    sb_putn(&text, src.s + i, open - i);
    if (next == '{') {
      int raw = (open + 2 < src.len && src.s[open + 2] == '{');
      const char *cs = raw ? "}}}" : "}}";
      size_t close = find_substr(src.s, src.len, open + (raw ? 3 : 2), cs);
      if (close == (size_t)-1) {
        sb_free(&text);
        *err = tmpl_err(a, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        return 0;
      }
      if (text.len) {
        Tok t; memset(&t, 0, sizeof t);
        t.kind = TK_TEXT; t.text = arena_str(a, text.buf, text.len);
        toklist_push(out, t);
        text.len = 0;
      }
      size_t ia = open + (raw ? 3 : 2), ib = close, ra, rb;
      trim_js_range(src.s, ia, ib, &ra, &rb);
      Str inner = str_make(src.s + ra, rb - ra);
      Str *path; int npath;
      if (!path_of(a, name, inner, &path, &npath, err)) { sb_free(&text); return 0; }
      Tok t; memset(&t, 0, sizeof t);
      t.kind = raw ? TK_RAW : TK_VAR; t.path = path; t.npath = npath;
      toklist_push(out, t);
      i = close + (raw ? 3 : 2);
      continue;
    }
    size_t close = find_substr(src.s, src.len, open + 2, "%}");
    if (close == (size_t)-1) {
      sb_free(&text);
      *err = tmpl_err(a, name, "a {% is not closed");
      return 0;
    }
    size_t end = close + 2;
    size_t line_start = 0;
    if (open > 0) {
      size_t q = open;
      while (q > 0 && src.s[q - 1] != '\n') q--;
      line_start = q;
    }
    size_t nl = find_substr(src.s, src.len, end, "\n");
    size_t line_end = nl == (size_t)-1 ? src.len : nl;
    int standalone = all_space_tab(src.s, line_start, open) && all_space_tab(src.s, end, line_end);
    if (standalone) text.len -= (open - line_start);
    if (text.len) {
      Tok t; memset(&t, 0, sizeof t);
      t.kind = TK_TEXT; t.text = arena_str(a, text.buf, text.len);
      toklist_push(out, t);
      text.len = 0;
    }
    size_t ra, rb;
    trim_js_range(src.s, open + 2, close, &ra, &rb);
    Tok t; memset(&t, 0, sizeof t);
    t.kind = TK_TAG; t.tag = str_make(src.s + ra, rb - ra);
    toklist_push(out, t);
    i = standalone ? (nl == (size_t)-1 ? src.len : nl + 1) : end;
  }
  if (text.len) {
    Tok t; memset(&t, 0, sizeof t);
    t.kind = TK_TEXT; t.text = arena_str(a, text.buf, text.len);
    toklist_push(out, t);
  }
  sb_free(&text);
  return 1;
}

/* FOR_RE: ^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$ ; IF_RE: ^if (not )?(\S+)$ */
static int parse_for_tag(Str tag, Str *varname, Str *pathtext) {
  if (tag.len < 5 || memcmp(tag.s, "for ", 4) != 0) return 0;
  size_t i = 4;
  if (i >= tag.len) return 0;
  char c = tag.s[i];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
  size_t vs = i;
  i++;
  while (i < tag.len) {
    c = tag.s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') i++;
    else break;
  }
  *varname = str_make(tag.s + vs, i - vs);
  if (i + 4 > tag.len || memcmp(tag.s + i, " in ", 4) != 0) return 0;
  i += 4;
  if (i >= tag.len) return 0;
  Str rest = str_make(tag.s + i, tag.len - i);
  if (has_js_ws(rest)) return 0;
  *pathtext = rest;
  return 1;
}
static int parse_if_tag(Str tag, int *not_, Str *pathtext) {
  if (tag.len < 4 || memcmp(tag.s, "if ", 3) != 0) return 0;
  size_t i = 3;
  *not_ = 0;
  if (tag.len - i >= 4 && memcmp(tag.s + i, "not ", 4) == 0) { *not_ = 1; i += 4; }
  if (i >= tag.len) return 0;
  Str rest = str_make(tag.s + i, tag.len - i);
  if (has_js_ws(rest)) return 0;
  *pathtext = rest;
  return 1;
}

static int parse_template(Arena *a, Str name, Str src, NodeList *root, Str *err) {
  TokList toks;
  if (!tokenize(a, name, src, &toks, err)) return 0;
  memset(root, 0, sizeof *root);
  typedef struct { Node *node; NodeList *body; } PFrame;
  PFrame stack[64];
  int sp = 0;
  stack[sp].node = NULL; stack[sp].body = root; sp++;
  for (size_t ti = 0; ti < toks.n; ti++) {
    Tok *tok = &toks.v[ti];
    NodeList *cur = stack[sp - 1].body;
    if (tok->kind == TK_TEXT) {
      Node *nd = node_new(a, N_TEXT);
      nd->text = tok->text;
      nodelist_push(cur, nd);
    } else if (tok->kind == TK_VAR || tok->kind == TK_RAW) {
      Node *nd = node_new(a, tok->kind == TK_VAR ? N_VAR : N_RAW);
      nd->path = tok->path; nd->npath = tok->npath;
      nodelist_push(cur, nd);
    } else {
      Str tag = tok->tag;
      Str varname, pathtext;
      int not_;
      if (parse_for_tag(tag, &varname, &pathtext)) {
        Str *path; int npath;
        if (!path_of(a, name, pathtext, &path, &npath, err)) { free(toks.v); return 0; }
        Node *nd = node_new(a, N_FOR);
        nd->varname = varname; nd->path = path; nd->npath = npath;
        nodelist_push(cur, nd);
        if (sp >= 64) { *err = tmpl_err(a, name, "template nesting too deep"); free(toks.v); return 0; }
        stack[sp].node = nd; stack[sp].body = &nd->body; sp++;
      } else if (parse_if_tag(tag, &not_, &pathtext)) {
        Str *path; int npath;
        if (!path_of(a, name, pathtext, &path, &npath, err)) { free(toks.v); return 0; }
        Node *nd = node_new(a, N_IF);
        nd->not_ = not_; nd->path = path; nd->npath = npath;
        nodelist_push(cur, nd);
        if (sp >= 64) { *err = tmpl_err(a, name, "template nesting too deep"); free(toks.v); return 0; }
        stack[sp].node = nd; stack[sp].body = &nd->body; sp++;
      } else if (str_eq_c(tag, "else")) {
        Node *top = stack[sp - 1].node;
        if (!top || top->kind != N_IF || top->has_else) {
          *err = tmpl_err(a, name, "an else outside an if");
          free(toks.v); return 0;
        }
        top->has_else = 1;
        stack[sp - 1].body = &top->els;
      } else if (str_eq_c(tag, "endfor") || str_eq_c(tag, "endif")) {
        int want_for = str_eq_c(tag, "endfor");
        Node *top = stack[sp - 1].node;
        if (!top || (want_for ? top->kind != N_FOR : top->kind != N_IF)) {
          char msg[96];
          snprintf(msg, sizeof msg, "an %s with no open %s", want_for ? "endfor" : "endif",
                   want_for ? "for" : "if");
          *err = tmpl_err(a, name, msg);
          free(toks.v); return 0;
        }
        sp--;
      } else if (tag.len >= 10 && memcmp(tag.s, "include \"", 9) == 0 &&
                 tag.s[tag.len - 1] == '"' ) {
        Str inner = str_make(tag.s + 9, tag.len - 10);
        int ok = inner.len > 0;
        for (size_t q = 0; q < inner.len && ok; q++) {
          char c = inner.s[q];
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
        }
        if (!ok) {
          StrBuf b; sb_init(&b);
          sb_put(&b, "{% ");
          sb_puts(&b, tag);
          sb_put(&b, " %} is not a tag");
          Str m = sb_to_arena(a, &b);
          StrBuf e; sb_init(&e);
          sb_put(&e, "template \""); sb_puts(&e, name); sb_put(&e, "\": "); sb_puts(&e, m);
          *err = sb_to_arena(a, &e);
          free(toks.v); return 0;
        }
        Node *nd = node_new(a, N_INCLUDE);
        nd->inc_name = inner;
        nodelist_push(cur, nd);
      } else {
        StrBuf b; sb_init(&b);
        sb_put(&b, "{% ");
        sb_puts(&b, tag);
        sb_put(&b, " %} is not a tag");
        Str m = sb_to_arena(a, &b);
        StrBuf e; sb_init(&e);
        sb_put(&e, "template \""); sb_puts(&e, name); sb_put(&e, "\": "); sb_puts(&e, m);
        *err = sb_to_arena(a, &e);
        free(toks.v); return 0;
      }
    }
  }
  free(toks.v);
  if (sp > 1) {
    Node *top = stack[sp - 1].node;
    char msg[64];
    snprintf(msg, sizeof msg, "a %s is not closed", top->kind == N_FOR ? "for" : "if");
    *err = tmpl_err(a, name, msg);
    return 0;
  }
  return 1;
}

/* ------------------------------------------------------------ template values */
typedef enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } VType;
typedef struct Val Val;
struct Val {
  VType t;
  int b;
  double num;
  Str str;
  Val **items; size_t n, cap;
  Str *keys; Val **vals; size_t nprops, capprops;
};
static Val VAL_UNDEF = {V_UNDEF, 0, 0, {0, 0}, 0, 0, 0, 0, 0, 0};
static Val VAL_NULL = {V_NULL, 0, 0, {0, 0}, 0, 0, 0, 0, 0, 0};
static Val *val_new(Arena *a, VType t) {
  Val *v = arena_alloc(a, sizeof(Val));
  memset(v, 0, sizeof *v);
  v->t = t;
  return v;
}
static Val *val_bool(Arena *a, int b) { Val *v = val_new(a, V_BOOL); v->b = !!b; return v; }
static Val *val_num(Arena *a, double n) { Val *v = val_new(a, V_NUM); v->num = n; return v; }
static Val *val_str(Arena *a, Str s) { Val *v = val_new(a, V_STR); v->str = s; return v; }
static Val *val_arr(Arena *a) { return val_new(a, V_ARR); }
static Val *val_obj(Arena *a) { return val_new(a, V_OBJ); }
static void val_arr_push(Val *arr, Val *it) {
  if (arr->n == arr->cap) {
    arr->cap = arr->cap ? arr->cap * 2 : 8;
    arr->items = realloc(arr->items, arr->cap * sizeof(Val *));
    if (!arr->items) exit(1);
  }
  arr->items[arr->n++] = it;
}
static void val_obj_set(Val *o, Str key, Val *v) {
  for (size_t i = 0; i < o->nprops; i++)
    if (str_eq(o->keys[i], key)) { o->vals[i] = v; return; }
  if (o->nprops == o->capprops) {
    o->capprops = o->capprops ? o->capprops * 2 : 8;
    o->keys = realloc(o->keys, o->capprops * sizeof(Str));
    o->vals = realloc(o->vals, o->capprops * sizeof(Val *));
    if (!o->keys || !o->vals) exit(1);
  }
  o->keys[o->nprops] = key;
  o->vals[o->nprops] = v;
  o->nprops++;
}
static Val *val_obj_get(Val *o, Str key) {
  if (!o || o->t != V_OBJ) return NULL;
  for (size_t i = 0; i < o->nprops; i++)
    if (str_eq(o->keys[i], key)) return o->vals[i];
  return NULL;
}
static void text_of(Val *v, StrBuf *out) {
  if (!v) return;
  switch (v->t) {
    case V_STR: sb_puts(out, v->str); break;
    case V_NUM: fmt_js_number(v->num, out); break;
    case V_BOOL: sb_put(out, v->b ? "true" : "false"); break;
    case V_ARR:
      for (size_t i = 0; i < v->n; i++) {
        if (i) sb_put(out, ", ");
        text_of(v->items[i], out);
      }
      break;
    default: break;
  }
}
static int truthy(Val *v) {
  if (!v) return 0;
  switch (v->t) {
    case V_ARR: return v->n > 0;
    case V_UNDEF: case V_NULL: return 0;
    case V_BOOL: return v->b;
    case V_STR: return v->str.len > 0;
    case V_NUM: return v->num != 0;
    default: return 1;
  }
}
typedef struct Frame Frame;
struct Frame { Val *obj; Frame *parent; };
static Val *lk_val(Arena *a, Val *v, Str key) {
  if (!v) return &VAL_UNDEF;
  if (v->t == V_OBJ) {
    Val *r = val_obj_get(v, key);
    return r ? r : &VAL_UNDEF;
  }
  if (v->t == V_ARR && str_eq_c(key, "length")) return val_num(a, (double)v->n);
  if (v->t == V_STR && str_eq_c(key, "length")) return val_num(a, (double)js_len(v->str));
  return &VAL_UNDEF;
}
static Val *lookup_val(Arena *a, Frame *f, Str *path, int npath) {
  Val *v = NULL;
  for (Frame *fr = f; fr; fr = fr->parent) {
    v = val_obj_get(fr->obj, path[0]);
    if (v) break;
  }
  if (!v) return &VAL_UNDEF;
  for (int k = 1; k < npath; k++) v = lk_val(a, v, path[k]);
  return v;
}

typedef struct { Str name; NodeList body; } Tmpl;
/* transitive first-segment scope names a template reads (computeDeps). */
typedef struct {
  Str name; int kind; /* 0 undef/null, 1 obj/arr ptr, 2 str, 3 num, 4 bool */
  Val *ptr; double num; Str str; int b;
} IncKeyVal;
typedef struct {
  Tmpl *tmpl; int depth;
  IncKeyVal *kv; size_t nkv;
  Str result;
} IncCacheEnt;
typedef struct {
  Tmpl *tmpls; size_t ntmpls; Arena *arena;
  Str **deps; size_t *ndeps; /* per-template dep names */
  IncCacheEnt *cache; size_t ncache, capcache;
} TplCtx;
static Tmpl *tpl_find(TplCtx *ctx, Str name) {
  for (size_t i = 0; i < ctx->ntmpls; i++)
    if (str_eq(ctx->tmpls[i].name, name)) return &ctx->tmpls[i];
  return NULL;
}
static long tpl_index(TplCtx *ctx, Str name) {
  for (size_t i = 0; i < ctx->ntmpls; i++)
    if (str_eq(ctx->tmpls[i].name, name)) return (long)i;
  return -1;
}
/* collectDeps: first-segment names nodes read, minus bound names. */
static int set_has(Str *set, size_t n, Str v) {
  for (size_t i = 0; i < n; i++) if (str_eq(set[i], v)) return 1;
  return 0;
}
static void set_add(Str **set, size_t *n, size_t *cap, Str v) {
  if (set_has(*set, *n, v)) return;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 8;
    *set = realloc(*set, *cap * sizeof(Str));
    if (!*set) exit(1);
  }
  (*set)[(*n)++] = v;
}
static void collect_deps(TplCtx *ctx, NodeList *nodes, Str *bound, size_t nbound,
                         Str **out, size_t *nout, size_t *capout) {
  for (size_t i = 0; i < nodes->n; i++) {
    Node *nd = nodes->v[i];
    if (nd->kind == N_VAR || nd->kind == N_RAW || nd->kind == N_IF) {
      if (!set_has(bound, nbound, nd->path[0]))
        set_add(out, nout, capout, nd->path[0]);
    } else if (nd->kind == N_FOR) {
      if (!set_has(bound, nbound, nd->path[0]))
        set_add(out, nout, capout, nd->path[0]);
      Str *b2 = malloc((nbound + 2) * sizeof(Str));
      for (size_t k = 0; k < nbound; k++) b2[k] = bound[k];
      size_t nb2 = nbound;
      if (!set_has(b2, nb2, nd->varname)) b2[nb2++] = nd->varname;
      if (!set_has(b2, nb2, str_c("loop"))) b2[nb2++] = str_c("loop");
      collect_deps(ctx, &nd->body, b2, nb2, out, nout, capout);
      free(b2);
    } else if (nd->kind == N_INCLUDE) {
      long idx = tpl_index(ctx, nd->inc_name);
      if (idx < 0) continue;
      for (size_t k = 0; k < ctx->ndeps[idx]; k++)
        if (!set_has(bound, nbound, ctx->deps[idx][k]))
          set_add(out, nout, capout, ctx->deps[idx][k]);
    }
  }
}
static void compute_deps(TplCtx *ctx) {
  ctx->deps = calloc(ctx->ntmpls ? ctx->ntmpls : 1, sizeof(Str *));
  ctx->ndeps = calloc(ctx->ntmpls ? ctx->ntmpls : 1, sizeof(size_t));
  size_t *caps = calloc(ctx->ntmpls ? ctx->ntmpls : 1, sizeof(size_t));
  int changed = 1, iter = 0;
  while (changed && iter++ < 20) {
    changed = 0;
    for (size_t i = 0; i < ctx->ntmpls; i++) {
      size_t before = ctx->ndeps[i];
      collect_deps(ctx, &ctx->tmpls[i].body, NULL, 0, &ctx->deps[i], &ctx->ndeps[i], &caps[i]);
      if (ctx->ndeps[i] != before) changed = 1;
    }
  }
  free(caps);
}
static Val *resolve_first_seg(Frame *f, Str name) {
  for (Frame *fr = f; fr; fr = fr->parent) {
    Val *v = val_obj_get(fr->obj, name);
    if (v) return v;
  }
  return &VAL_UNDEF;
}
static void render_nodes(NodeList *nodes, Frame *f, TplCtx *ctx, int depth, StrBuf *out) {
  for (size_t ni = 0; ni < nodes->n; ni++) {
    Node *nd = nodes->v[ni];
    switch (nd->kind) {
      case N_TEXT: sb_puts(out, nd->text); break;
      case N_VAR: {
        Val *v = lookup_val(ctx->arena, f, nd->path, nd->npath);
        if (v->t == V_STR) esc_html_range(out, v->str.s, 0, v->str.len);
        else {
          StrBuf tmp; sb_init(&tmp);
          text_of(v, &tmp);
          esc_html_range(out, tmp.buf ? tmp.buf : "", 0, tmp.len);
          sb_free(&tmp);
        }
        break;
      }
      case N_RAW: {
        Val *v = lookup_val(ctx->arena, f, nd->path, nd->npath);
        text_of(v, out);
        break;
      }
      case N_IF: {
        Val *v = lookup_val(ctx->arena, f, nd->path, nd->npath);
        int tv = truthy(v);
        NodeList *br = (tv != nd->not_) ? &nd->body : (nd->has_else ? &nd->els : NULL);
        if (br) render_nodes(br, f, ctx, depth, out);
        break;
      }
      case N_FOR: {
        Val *xs = lookup_val(ctx->arena, f, nd->path, nd->npath);
        if (!xs || xs->t != V_ARR) break;
        for (size_t k = 0; k < xs->n; k++) {
          Val *scope = val_obj(ctx->arena);
          val_obj_set(scope, nd->varname, xs->items[k]);
          Val *loop = val_obj(ctx->arena);
          val_obj_set(loop, str_c("index"), val_num(ctx->arena, (double)(k + 1)));
          val_obj_set(loop, str_c("first"), val_bool(ctx->arena, k == 0));
          val_obj_set(loop, str_c("last"), val_bool(ctx->arena, k == xs->n - 1));
          val_obj_set(loop, str_c("length"), val_num(ctx->arena, (double)xs->n));
          val_obj_set(scope, str_c("loop"), loop);
          Frame f2 = {scope, f};
          render_nodes(&nd->body, &f2, ctx, depth, out);
        }
        break;
      }
      case N_INCLUDE: {
        Tmpl *t = tpl_find(ctx, nd->inc_name);
        if (t && depth < 8) {
          /* Memoized include: keyed by template, depth, and the values of
           * every scope name the template transitively reads. */
          long tidx = tpl_index(ctx, nd->inc_name);
          size_t nkv = tidx < 0 ? 0 : ctx->ndeps[tidx];
          IncKeyVal kv[32];
          if (nkv > 32) nkv = 32; /* deps are few; fall back to direct below if more */
          int use_cache = tidx >= 0 && ctx->ndeps[tidx] <= 32;
          if (use_cache) {
            for (size_t k = 0; k < nkv; k++) {
              Val *v = resolve_first_seg(f, ctx->deps[tidx][k]);
              kv[k].name = ctx->deps[tidx][k];
              kv[k].ptr = v;
              switch (v->t) {
                case V_OBJ: case V_ARR: kv[k].kind = 1; break;
                case V_STR: kv[k].kind = 2; kv[k].str = v->str; break;
                case V_NUM: kv[k].kind = 3; kv[k].num = v->num; break;
                case V_BOOL: kv[k].kind = 4; kv[k].b = v->b; break;
                default: kv[k].kind = 0; break;
              }
            }
            IncCacheEnt *hit = NULL;
            for (size_t e = 0; e < ctx->ncache; e++) {
              IncCacheEnt *en = &ctx->cache[e];
              if (en->tmpl != t || en->depth != depth || en->nkv != nkv) continue;
              int eq = 1;
              for (size_t k = 0; k < nkv; k++) {
                if (en->kv[k].kind != kv[k].kind) { eq = 0; break; }
                if (kv[k].kind == 1 && en->kv[k].ptr != kv[k].ptr) { eq = 0; break; }
                if (kv[k].kind == 2 && !str_eq(en->kv[k].str, kv[k].str)) { eq = 0; break; }
                if (kv[k].kind == 3 && en->kv[k].num != kv[k].num) { eq = 0; break; }
                if (kv[k].kind == 4 && en->kv[k].b != kv[k].b) { eq = 0; break; }
              }
              if (eq) { hit = en; break; }
            }
            if (hit) { sb_puts(out, hit->result); break; }
          }
          StrBuf tmp; sb_init(&tmp);
          render_nodes(&t->body, f, ctx, depth + 1, &tmp);
          Str res = sb_to_arena(ctx->arena, &tmp);
          sb_puts(out, res);
          if (use_cache) {
            if (ctx->ncache == ctx->capcache) {
              ctx->capcache = ctx->capcache ? ctx->capcache * 2 : 64;
              ctx->cache = realloc(ctx->cache, ctx->capcache * sizeof(IncCacheEnt));
              if (!ctx->cache) exit(1);
            }
            IncCacheEnt *en = &ctx->cache[ctx->ncache++];
            en->tmpl = t; en->depth = depth; en->nkv = nkv;
            en->kv = malloc((nkv ? nkv : 1) * sizeof(IncKeyVal));
            for (size_t k = 0; k < nkv; k++) en->kv[k] = kv[k];
            en->result = res;
          }
        }
        break;
      }
    }
  }
}
/* render template by name with root scope; 1 ok (result in *res), 0 missing. */
static int render_tpl(TplCtx *ctx, Str name, Frame *root, Str *res) {
  Tmpl *t = tpl_find(ctx, name);
  if (!t) return 0;
  StrBuf out; sb_init(&out);
  render_nodes(&t->body, root, ctx, 0, &out);
  *res = sb_to_arena(ctx->arena, &out);
  return 1;
}

/* ------------------------------------------------------------------ build */
typedef struct Source Source;
struct Source {
  Str path, text, body;
  FMData fm;
  int post;
  Str layout;
  Str slug, out_dir, url;
  Str html, excerpt;
  TocItem *toc; size_t ntoc;
  Source *newer, *older;
  Val **tag_refs; size_t ntag_refs; /* tag objects (Val*) */
  Val *page_val;
};
typedef struct { Str name, slug, url; Source **posts; size_t nposts, capposts; } Tag;
typedef struct { Str path, content; } StaticFile;
typedef struct { Str target; int is_null; Str resolved; } LinkEnt;
typedef struct { Str dir; LinkEnt *ents; size_t n, cap; } DirCache;
typedef struct {
  Arena *arena;
  Str base;
  TplCtx tpl;
  Str *report; size_t nreport, capreport;
  Source **sources; size_t nsources, capsources;
  StaticFile *statics; size_t nstatics, capstatics;
  Tag **tags; size_t ntags, captags;
  Source **posts; size_t nposts, capposts;
  Str *broken; size_t nbroken, capbroken;
  Str *out_keys; Str *out_vals; size_t nout, capout;
  SlugCache slugcache;
  DirCache *dcs; size_t ndcs, capdcs;
} Build;

static void build_report_push(Build *b, Str line) {
  if (b->nreport == b->capreport) {
    b->capreport = b->capreport ? b->capreport * 2 : 32;
    b->report = realloc(b->report, b->capreport * sizeof(Str));
    if (!b->report) exit(1);
  }
  b->report[b->nreport++] = line;
}
static void build_broken_add(Build *b, Str line) {
  for (size_t i = 0; i < b->nbroken; i++) if (str_eq(b->broken[i], line)) return;
  if (b->nbroken == b->capbroken) {
    b->capbroken = b->capbroken ? b->capbroken * 2 : 32;
    b->broken = realloc(b->broken, b->capbroken * sizeof(Str));
    if (!b->broken) exit(1);
  }
  b->broken[b->nbroken++] = line;
}
static void build_out_add(Build *b, Str key, Str val) {
  if (b->nout == b->capout) {
    b->capout = b->capout ? b->capout * 2 : 64;
    b->out_keys = realloc(b->out_keys, b->capout * sizeof(Str));
    b->out_vals = realloc(b->out_vals, b->capout * sizeof(Str));
    if (!b->out_keys || !b->out_vals) exit(1);
  }
  b->out_keys[b->nout] = key;
  b->out_vals[b->nout] = val;
  b->nout++;
}
static int build_out_has(Build *b, Str key) {
  for (size_t i = 0; i < b->nout; i++) if (str_eq(b->out_keys[i], key)) return 1;
  return 0;
}
static Str dir_of(Arena *a, Str path) {
  for (size_t i = path.len; i > 0; i--)
    if (path.s[i - 1] == '/') return arena_str(a, path.s, i - 1);
  return str_c("");
}
/* resolvePath(dir, rel): 1 ok (in *out), 0 null (too many ..). */
static int resolve_path(Arena *a, Str dir, Str rel, Str *out) {
  Str *plist = malloc(sizeof(Str) * 256);
  size_t np = 0, cp = 256;
  if (dir.len > 0) {
    size_t start = 0;
    for (size_t i = 0; i <= dir.len; i++) {
      if (i == dir.len || dir.s[i] == '/') {
        if (np == cp) { cp *= 2; plist = realloc(plist, cp * sizeof(Str)); }
        plist[np++] = str_make(dir.s + start, i - start);
        start = i + 1;
      }
    }
  }
  size_t start = 0;
  for (size_t i = 0; i <= rel.len; i++) {
    if (i == rel.len || rel.s[i] == '/') {
      Str seg = str_make(rel.s + start, i - start);
      if (seg.len == 0 || str_eq_c(seg, ".")) { /* skip */ }
      else if (str_eq_c(seg, "..")) {
        if (np == 0) { free(plist); return 0; }
        np--;
      } else {
        if (np == cp) { cp *= 2; plist = realloc(plist, cp * sizeof(Str)); }
        plist[np++] = seg;
      }
      start = i + 1;
    }
  }
  StrBuf b; sb_init(&b);
  for (size_t i = 0; i < np; i++) {
    if (i) sb_putc(&b, '/');
    sb_puts(&b, plist[i]);
  }
  free(plist);
  *out = sb_to_arena(a, &b);
  return 1;
}

typedef struct { Build *b; Source *src; Str dir; } LinkCtx;
static Source *by_source(Build *b, Str path) {
  for (size_t i = 0; i < b->nsources; i++)
    if (str_eq(b->sources[i]->path, path)) return b->sources[i];
  return NULL;
}
static int static_has(Build *b, Str path) {
  for (size_t i = 0; i < b->nstatics; i++)
    if (str_eq(b->statics[i].path, path)) return 1;
  return 0;
}
static Str link_resolve(void *vctx, Str target) {
  LinkCtx *ctx = vctx;
  Build *b = ctx->b;
  Arena *a = b->arena;
  /* per-dir cache */
  DirCache *dc = NULL;
  for (size_t i = 0; i < b->ndcs; i++)
    if (str_eq(b->dcs[i].dir, ctx->dir)) { dc = &b->dcs[i]; break; }
  if (!dc) {
    if (b->ndcs == b->capdcs) {
      b->capdcs = b->capdcs ? b->capdcs * 2 : 8;
      b->dcs = realloc(b->dcs, b->capdcs * sizeof(DirCache));
      if (!b->dcs) exit(1);
    }
    dc = &b->dcs[b->ndcs++];
    memset(dc, 0, sizeof *dc);
    dc->dir = ctx->dir;
  }
  for (size_t i = 0; i < dc->n; i++) {
    if (str_eq(dc->ents[i].target, target)) {
      if (dc->ents[i].is_null) {
        StrBuf lb; sb_init(&lb);
        sb_put(&lb, "broken-link ");
        sb_puts(&lb, ctx->src->path);
        sb_put(&lb, " -> ");
        sb_puts(&lb, target);
        build_broken_add(b, sb_to_arena(a, &lb));
        return target;
      }
      return dc->ents[i].resolved;
    }
  }
  Str out;
  int broken = 0;
  int scheme = 0;
  if (target.len > 0) {
    unsigned char c0 = (unsigned char)target.s[0];
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
      size_t j = 1;
      while (j < target.len) {
        unsigned char c = (unsigned char)target.s[j];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '-' || c == '.') j++;
        else break;
      }
      scheme = j < target.len && target.s[j] == ':';
    }
  }
  if (scheme || starts_with(target, "/") || starts_with(target, "#")) {
    out = target;
  } else {
    size_t hash = (size_t)-1;
    for (size_t i = 0; i < target.len; i++) if (target.s[i] == '#') { hash = i; break; }
    Str rel = hash == (size_t)-1 ? target : str_make(target.s, hash);
    Str frag = hash == (size_t)-1 ? str_c("") : str_make(target.s + hash, target.len - hash);
    Str path;
    int have_path = rel.len > 0 && resolve_path(a, ctx->dir, rel, &path);
    Source *hit = have_path ? by_source(b, path) : NULL;
    if (hit) out = arena_str_cat(a, hit->url, frag);
    else if (have_path && static_has(b, path)) {
      StrBuf sb; sb_init(&sb);
      sb_puts(&sb, b->base); sb_putc(&sb, '/'); sb_puts(&sb, path); sb_puts(&sb, frag);
      out = sb_to_arena(a, &sb);
    } else broken = 1;
  }
  if (dc->n == dc->cap) {
    dc->cap = dc->cap ? dc->cap * 2 : 16;
    dc->ents = realloc(dc->ents, dc->cap * sizeof(LinkEnt));
    if (!dc->ents) exit(1);
  }
  dc->ents[dc->n].target = target;
  dc->ents[dc->n].is_null = broken;
  dc->ents[dc->n].resolved = broken ? target : out;
  dc->n++;
  if (broken) {
    StrBuf lb; sb_init(&lb);
    sb_put(&lb, "broken-link ");
    sb_puts(&lb, ctx->src->path);
    sb_put(&lb, " -> ");
    sb_puts(&lb, target);
    build_broken_add(b, sb_to_arena(a, &lb));
    return target;
  }
  return out;
}

/* insertion sort (stable) helpers */
static int cmp_file_path(const void *pa, const void *pb) {
  JVal *x = *(JVal **)pa, *y = *(JVal **)pb;
  Str px = jobj_get(x, "path")->str, py = jobj_get(y, "path")->str;
  return cmp_js_str(px, py);
}
static int cmp_strp(const void *pa, const void *pb) {
  Str x = *(Str *)pa, y = *(Str *)pb;
  return cmp_js_str(x, y);
}
static void isort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
  char *arr = base, *tmp = malloc(sz);
  for (size_t i = 1; i < n; i++) {
    memcpy(tmp, arr + i * sz, sz);
    size_t j = i;
    while (j > 0 && cmp(arr + (j - 1) * sz, tmp) > 0) {
      memcpy(arr + j * sz, arr + (j - 1) * sz, sz);
      j--;
    }
    memcpy(arr + j * sz, tmp, sz);
  }
  free(tmp);
}

static Str normalize_cr(Arena *a, Str c) {
  if (!memchr(c.s, '\r', c.len)) return c;
  StrBuf b; sb_init(&b);
  for (size_t i = 0; i < c.len; i++) {
    if (c.s[i] == '\r') {
      sb_putc(&b, '\n');
      if (i + 1 < c.len && c.s[i + 1] == '\n') i++;
    } else sb_putc(&b, c.s[i]);
  }
  return sb_to_arena(a, &b);
}

static Tag *tag_by_slug(Build *b, Str slug) {
  for (size_t i = 0; i < b->ntags; i++)
    if (str_eq(b->tags[i]->slug, slug)) return b->tags[i];
  return NULL;
}
static Tag *tag_get_or_create(Build *b, Str name, Str slug) {
  Tag *t = tag_by_slug(b, slug);
  if (t) return t;
  t = arena_alloc(b->arena, sizeof(Tag));
  memset(t, 0, sizeof *t);
  t->name = name; t->slug = slug;
  StrBuf u; sb_init(&u);
  sb_puts(&u, b->base); sb_put(&u, "/tags/"); sb_puts(&u, slug); sb_putc(&u, '/');
  t->url = sb_to_arena(b->arena, &u);
  if (b->ntags == b->captags) {
    b->captags = b->captags ? b->captags * 2 : 16;
    b->tags = realloc(b->tags, b->captags * sizeof(Tag *));
    if (!b->tags) exit(1);
  }
  b->tags[b->ntags++] = t;
  return t;
}
static void tag_add_post(Tag *t, Source *s) {
  if (t->nposts == t->capposts) {
    t->capposts = t->capposts ? t->capposts * 2 : 8;
    t->posts = realloc(t->posts, t->capposts * sizeof(Source *));
    if (!t->posts) exit(1);
  }
  t->posts[t->nposts++] = s;
}

/* ownTags: dedupe fm tags by slug, first spelling kept. Returns name list. */
static Str *own_tag_names(Build *b, Source *s, size_t *out_n) {
  if (!s->fm.has_tags || s->fm.ntags == 0) { *out_n = 0; return NULL; }
  Str *names = malloc(s->fm.ntags * sizeof(Str));
  Str *slugs = malloc(s->fm.ntags * sizeof(Str));
  size_t n = 0;
  for (size_t i = 0; i < s->fm.ntags; i++) {
    Str slug = slugify(b->arena, &b->slugcache, s->fm.tags[i]);
    int seen = 0;
    for (size_t j = 0; j < n; j++) if (str_eq(slugs[j], slug)) { seen = 1; break; }
    if (!seen) { slugs[n] = slug; names[n] = s->fm.tags[i]; n++; }
  }
  free(slugs);
  *out_n = n;
  return names;
}

static Val *link_of_val(Build *b, Source *s) {
  if (!s) return &VAL_NULL;
  Val *o = val_obj(b->arena);
  val_obj_set(o, str_c("title"), val_str(b->arena, s->fm.title));
  val_obj_set(o, str_c("url"), val_str(b->arena, s->url));
  val_obj_set(o, str_c("date"), val_str(b->arena, s->fm.has_date ? s->fm.date : str_c("")));
  val_obj_set(o, str_c("date_long"),
              val_str(b->arena, s->fm.has_date ? long_date(b->arena, s->fm.date) : str_c("")));
  return o;
}
static Val *tag_ref_val(Build *b, Tag *t) {
  Val *o = val_obj(b->arena);
  val_obj_set(o, str_c("name"), val_str(b->arena, t->name));
  val_obj_set(o, str_c("slug"), val_str(b->arena, t->slug));
  val_obj_set(o, str_c("url"), val_str(b->arena, t->url));
  Val *posts = val_arr(b->arena);
  for (size_t i = 0; i < t->nposts; i++) val_arr_push(posts, t->posts[i]->page_val);
  val_obj_set(o, str_c("posts"), posts);
  return o;
}

static void build_case(Build *b, JVal *input) {
  Arena *a = b->arena;
  JVal *site = jobj_get(input, "site");
  Str title = jobj_get(site, "title")->str;
  Str base_url = jobj_get(site, "base_url")->str;
  long per_page = (long)jobj_get(site, "per_page")->num;
  long recent_n = (long)jobj_get(site, "recent")->num;
  /* base: strip trailing slashes */
  size_t bl = base_url.len;
  while (bl > 0 && base_url.s[bl - 1] == '/') bl--;
  b->base = str_make(base_url.s, bl);

  /* templates, sorted by name */
  JVal *tmpls = jobj_get(input, "templates");
  Str *tnames = malloc((tmpls->n ? tmpls->n : 1) * sizeof(Str));
  for (size_t i = 0; i < tmpls->n; i++) tnames[i] = tmpls->keys[i];
  isort(tnames, tmpls->n, sizeof(Str), cmp_strp);
  b->tpl.arena = a;
  b->tpl.tmpls = malloc((tmpls->n ? tmpls->n : 1) * sizeof(Tmpl));
  b->tpl.ntmpls = 0;
  for (size_t i = 0; i < tmpls->n; i++) {
    JVal *tv = NULL;
    for (size_t k = 0; k < tmpls->n; k++)
      if (str_eq(tmpls->keys[k], tnames[i])) { tv = tmpls->vals[k]; break; }
    NodeList root;
    Str err;
    if (!parse_template(a, tnames[i], tv->str, &root, &err)) {
      build_report_push(b, err);
      continue;
    }
    b->tpl.tmpls[b->tpl.ntmpls].name = tnames[i];
    b->tpl.tmpls[b->tpl.ntmpls].body = root;
    b->tpl.ntmpls++;
  }
  free(tnames);
  compute_deps(&b->tpl);

  /* 1. sources, sorted by path */
  JVal *files = jobj_get(input, "files");
  JVal **fsorted = malloc((files->nitems ? files->nitems : 1) * sizeof(JVal *));
  for (size_t i = 0; i < files->nitems; i++) fsorted[i] = files->items[i];
  isort(fsorted, files->nitems, sizeof(JVal *), cmp_file_path);
  Source **all_sources = malloc((files->nitems ? files->nitems : 1) * sizeof(Source *));
  size_t nall = 0;
  for (size_t i = 0; i < files->nitems; i++) {
    Str path = jobj_get(fsorted[i], "path")->str;
    Str content = jobj_get(fsorted[i], "content")->str;
    if (ends_with(path, ".md")) {
      Source *s = arena_alloc(a, sizeof(Source));
      memset(s, 0, sizeof *s);
      s->path = path;
      s->text = normalize_cr(a, content);
      all_sources[nall++] = s;
    } else {
      if (b->nstatics == b->capstatics) {
        b->capstatics = b->capstatics ? b->capstatics * 2 : 16;
        b->statics = realloc(b->statics, b->capstatics * sizeof(StaticFile));
        if (!b->statics) exit(1);
      }
      b->statics[b->nstatics].path = path;
      b->statics[b->nstatics].content = content;
      b->nstatics++;
    }
  }
  free(fsorted);

  /* 2. front matter */
  for (size_t si = 0; si < nall; si++) {
    Source *s = all_sources[si];
    Str body, err;
    if (!parse_front_matter(a, s->text, &s->fm, &body, &err)) {
      StrBuf r; sb_init(&r);
      sb_put(&r, "refused "); sb_puts(&r, s->path); sb_put(&r, ": "); sb_puts(&r, err);
      build_report_push(b, sb_to_arena(a, &r));
      continue;
    }
    s->body = body;
    s->post = starts_with(s->path, "posts/");
    const char *own = NULL;
    if (starts_with(s->path, "tags/")) own = "tags";
    else if (starts_with(s->path, "page/")) own = "page";
    if (own) {
      StrBuf r; sb_init(&r);
      sb_put(&r, "refused "); sb_puts(&r, s->path); sb_put(&r, ": ");
      sb_put(&r, own); sb_put(&r, "/ is the generator's own");
      build_report_push(b, sb_to_arena(a, &r));
      continue;
    }
    if (s->post && !s->fm.has_date) {
      StrBuf r; sb_init(&r);
      sb_put(&r, "refused "); sb_puts(&r, s->path); sb_put(&r, ": a post needs a date");
      build_report_push(b, sb_to_arena(a, &r));
      continue;
    }
    Str layout = s->fm.has_layout ? s->fm.layout : (s->post ? str_c("post") : str_c("page"));
    if (!tpl_find(&b->tpl, layout)) {
      StrBuf r; sb_init(&r);
      sb_put(&r, "refused "); sb_puts(&r, s->path); sb_put(&r, ": layout \"");
      sb_puts(&r, layout); sb_put(&r, "\" is no template that parses");
      build_report_push(b, sb_to_arena(a, &r));
      continue;
    }
    s->layout = layout;
    if (s->fm.has_draft && s->fm.draft) {
      StrBuf r; sb_init(&r);
      sb_put(&r, "draft "); sb_puts(&r, s->path);
      build_report_push(b, sb_to_arena(a, &r));
      continue;
    }
    if (b->nsources == b->capsources) {
      b->capsources = b->capsources ? b->capsources * 2 : 64;
      b->sources = realloc(b->sources, b->capsources * sizeof(Source *));
      if (!b->sources) exit(1);
    }
    b->sources[b->nsources++] = s;
  }
  free(all_sources);

  /* 3. permalinks */
  typedef struct { Str dir; StrSet set; } TakenDir;
  TakenDir *taken = malloc(sizeof(TakenDir) * 16);
  size_t ntaken = 0, captaken = 16;
  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    Str dir = s->post ? str_c("posts") : dir_of(a, s->path);
    TakenDir *td = NULL;
    for (size_t i = 0; i < ntaken; i++)
      if (str_eq(taken[i].dir, dir)) { td = &taken[i]; break; }
    if (!td) {
      if (ntaken == captaken) {
        captaken *= 2;
        taken = realloc(taken, captaken * sizeof(TakenDir));
        if (!taken) exit(1);
      }
      td = &taken[ntaken++];
      memset(td, 0, sizeof *td);
      td->dir = dir;
      if (dir.len == 0) {
        strset_add(&td->set, str_c("posts"));
        strset_add(&td->set, str_c("tags"));
        strset_add(&td->set, str_c("page"));
      }
    }
    Str slug = unique_slug(a, s->fm.has_slug ? s->fm.slug : slugify(a, &b->slugcache, s->fm.title),
                           &td->set);
    s->slug = slug;
    StrBuf od; sb_init(&od);
    if (dir.len > 0) { sb_puts(&od, dir); sb_putc(&od, '/'); }
    sb_puts(&od, slug);
    s->out_dir = sb_to_arena(a, &od);
    StrBuf u; sb_init(&u);
    sb_puts(&u, b->base); sb_putc(&u, '/'); sb_puts(&u, s->out_dir); sb_putc(&u, '/');
    s->url = sb_to_arena(a, &u);
  }
  free(taken);

  /* 4. markdown + link resolution */
  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    LinkCtx lctx = {b, s, dir_of(a, s->path)};
    MdEnv env = {link_resolve, &lctx};
    MdResult md = render_markdown(a, s->body, &env, &b->slugcache);
    s->html = md.html;
    s->toc = md.toc; s->ntoc = md.ntoc;
    s->excerpt = md.excerpt;
  }

  /* 5. collections */
  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    if (!s->post) continue;
    if (b->nposts == b->capposts) {
      b->capposts = b->capposts ? b->capposts * 2 : 64;
      b->posts = realloc(b->posts, b->capposts * sizeof(Source *));
      if (!b->posts) exit(1);
    }
    b->posts[b->nposts++] = s;
  }
  /* sort posts: date desc, slug asc (stable insertion sort) */
  for (size_t i = 1; i < b->nposts; i++) {
    Source *tmp = b->posts[i];
    size_t j = i;
    while (j > 0) {
      Source *pv = b->posts[j - 1];
      int c = cmp_js_str(tmp->fm.date, pv->fm.date);
      if (c == 0) c = cmp_js_str(tmp->slug, pv->slug);
      else c = -c; /* date desc */
      if (c >= 0) break;
      b->posts[j] = pv;
      j--;
    }
    b->posts[j] = tmp;
  }
  for (size_t k = 0; k < b->nposts; k++) {
    b->posts[k]->newer = k == 0 ? NULL : b->posts[k - 1];
    b->posts[k]->older = k == b->nposts - 1 ? NULL : b->posts[k + 1];
  }

  /* 6. tags */
  for (size_t pi = 0; pi < b->nposts; pi++) {
    Source *p = b->posts[pi];
    size_t nn;
    Str *names = own_tag_names(b, p, &nn);
    p->tag_refs = malloc((nn ? nn : 1) * sizeof(Val *));
    p->ntag_refs = nn;
    for (size_t i = 0; i < nn; i++) {
      Str slug = slugify(a, &b->slugcache, names[i]);
      Tag *t = tag_get_or_create(b, names[i], slug);
      tag_add_post(t, p);
      p->tag_refs[i] = NULL; /* filled after page_vals exist */
    }
    free(names);
  }
  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    if (s->post) continue;
    size_t nn;
    Str *names = own_tag_names(b, s, &nn);
    s->tag_refs = malloc((nn ? nn : 1) * sizeof(Val *));
    s->ntag_refs = nn;
    for (size_t i = 0; i < nn; i++) {
      Str slug = slugify(a, &b->slugcache, names[i]);
      Tag *t = tag_by_slug(b, slug);
      if (t) s->tag_refs[i] = NULL; /* real tag, filled later */
      else {
        Val *ph = val_obj(a);
        val_obj_set(ph, str_c("name"), val_str(a, names[i]));
        val_obj_set(ph, str_c("slug"), val_str(a, slug));
        val_obj_set(ph, str_c("url"), val_str(a, str_c("")));
        val_obj_set(ph, str_c("posts"), val_arr(a));
        s->tag_refs[i] = ph;
      }
    }
    free(names);
  }
  /* sort tags: count desc, slug asc */
  for (size_t i = 1; i < b->ntags; i++) {
    Tag *tmp = b->tags[i];
    size_t j = i;
    while (j > 0) {
      Tag *pv = b->tags[j - 1];
      int c = 0;
      if (tmp->nposts != pv->nposts) c = tmp->nposts > pv->nposts ? -1 : 1;
      else c = cmp_js_str(tmp->slug, pv->slug);
      if (c >= 0) break;
      b->tags[j] = pv;
      j--;
    }
    b->tags[j] = tmp;
  }

  /* 7. layouts: attach template-visible fields */
  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    Val *o = val_obj(a);
    s->page_val = o;
    val_obj_set(o, str_c("title"), val_str(a, s->fm.title));
    val_obj_set(o, str_c("url"), val_str(a, s->url));
    val_obj_set(o, str_c("slug"), val_str(a, s->slug));
    val_obj_set(o, str_c("outDir"), val_str(a, s->out_dir));
    val_obj_set(o, str_c("path"), val_str(a, s->path));
    val_obj_set(o, str_c("body"), val_str(a, s->body));
    val_obj_set(o, str_c("html"), val_str(a, s->html));
    val_obj_set(o, str_c("post"), val_bool(a, s->post));
    val_obj_set(o, str_c("layout"), val_str(a, s->layout));
    val_obj_set(o, str_c("date"), val_str(a, s->fm.has_date ? s->fm.date : str_c("")));
    val_obj_set(o, str_c("date_long"),
                val_str(a, s->fm.has_date ? long_date(a, s->fm.date) : str_c("")));
    val_obj_set(o, str_c("summary"), val_str(a, s->fm.has_summary ? s->fm.summary : str_c("")));
    int rm = (words_of(s->body) + 199) / 200;
    if (rm < 1) rm = 1;
    val_obj_set(o, str_c("reading_minutes"), val_num(a, (double)rm));
    val_obj_set(o, str_c("content"), val_str(a, s->html));
    val_obj_set(o, str_c("excerpt"), val_str(a, s->excerpt));
    val_obj_set(o, str_c("is_post"), val_bool(a, s->post));
    Val *toc = val_arr(a);
    for (size_t k = 0; k < s->ntoc; k++) {
      Val *ti = val_obj(a);
      val_obj_set(ti, str_c("level"), val_num(a, (double)s->toc[k].level));
      val_obj_set(ti, str_c("id"), val_str(a, s->toc[k].id));
      val_obj_set(ti, str_c("html"), val_str(a, s->toc[k].html));
      val_arr_push(toc, ti);
    }
    val_obj_set(o, str_c("toc"), toc);
    /* data (front matter object) */
    Val *data = val_obj(a);
    if (s->fm.has_title) val_obj_set(data, str_c("title"), val_str(a, s->fm.title));
    if (s->fm.has_date) val_obj_set(data, str_c("date"), val_str(a, s->fm.date));
    if (s->fm.has_tags) {
      Val *ta = val_arr(a);
      for (size_t k = 0; k < s->fm.ntags; k++) val_arr_push(ta, val_str(a, s->fm.tags[k]));
      val_obj_set(data, str_c("tags"), ta);
    }
    if (s->fm.has_draft) val_obj_set(data, str_c("draft"), val_bool(a, s->fm.draft));
    if (s->fm.has_layout) val_obj_set(data, str_c("layout"), val_str(a, s->fm.layout));
    if (s->fm.has_slug) val_obj_set(data, str_c("slug"), val_str(a, s->fm.slug));
    if (s->fm.has_summary) val_obj_set(data, str_c("summary"), val_str(a, s->fm.summary));
    val_obj_set(o, str_c("data"), data);
    if (s->post) {
      val_obj_set(o, str_c("newer"), link_of_val(b, s->newer));
      val_obj_set(o, str_c("older"), link_of_val(b, s->older));
    } else {
      val_obj_set(o, str_c("newer"), &VAL_NULL);
      val_obj_set(o, str_c("older"), &VAL_NULL);
    }
  }
  /* fill tag_refs (now that page_vals exist) and page.tags */
  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    size_t nn;
    Str *names = own_tag_names(b, s, &nn);
    Val *tags_arr = val_arr(a);
    for (size_t i = 0; i < nn; i++) {
      Str slug = slugify(a, &b->slugcache, names[i]);
      Tag *t = tag_by_slug(b, slug);
      Val *ref;
      if (t) {
        ref = tag_ref_val(b, t);
        if (s->post) s->tag_refs[i] = ref;
        else if (!s->tag_refs[i]) s->tag_refs[i] = ref;
      } else ref = s->tag_refs[i]; /* placeholder (non-post only) */
      Val *mini = val_obj(a);
      val_obj_set(mini, str_c("name"), val_str(a, t ? t->name : names[i]));
      val_obj_set(mini, str_c("slug"), val_str(a, slug));
      val_obj_set(mini, str_c("url"), val_str(a, t ? t->url : str_c("")));
      val_arr_push(tags_arr, mini);
    }
    free(names);
    val_obj_set(s->page_val, str_c("tags"), tags_arr);
    Val *refs = val_arr(a);
    for (size_t i = 0; i < s->ntag_refs; i++) val_arr_push(refs, s->tag_refs[i]);
    val_obj_set(s->page_val, str_c("tagRefs"), refs);
  }

  Val *site_val = val_obj(a);
  val_obj_set(site_val, str_c("title"), val_str(a, title));
  val_obj_set(site_val, str_c("base_url"), val_str(a, b->base));
  Val *all_tags = val_arr(a);
  for (size_t i = 0; i < b->ntags; i++) {
    Val *t = val_obj(a);
    val_obj_set(t, str_c("name"), val_str(a, b->tags[i]->name));
    val_obj_set(t, str_c("slug"), val_str(a, b->tags[i]->slug));
    val_obj_set(t, str_c("url"), val_str(a, b->tags[i]->url));
    val_obj_set(t, str_c("count"), val_num(a, (double)b->tags[i]->nposts));
    val_arr_push(all_tags, t);
  }
  Val *recent = val_arr(a);
  for (long i = 0; i < recent_n && (size_t)i < b->nposts; i++)
    val_arr_push(recent, b->posts[i]->page_val);

  Val *root = val_obj(a);
  val_obj_set(root, str_c("site"), site_val);
  val_obj_set(root, str_c("tags"), all_tags);
  val_obj_set(root, str_c("recent"), recent);
  Frame root_frame = {root, NULL};

  for (size_t si = 0; si < b->nsources; si++) {
    Source *s = b->sources[si];
    val_obj_set(root, str_c("page"), s->page_val);
    val_obj_set(root, str_c("posts"), &VAL_UNDEF);
    val_obj_set(root, str_c("pager"), &VAL_UNDEF);
    val_obj_set(root, str_c("tag"), &VAL_UNDEF);
    Str html;
    if (render_tpl(&b->tpl, s->layout, &root_frame, &html)) {
      StrBuf k; sb_init(&k);
      sb_puts(&k, s->out_dir); sb_put(&k, "/index.html");
      build_out_add(b, sb_to_arena(a, &k), html);
    }
  }
  long pages = 1;
  if (per_page > 0) {
    pages = (long)((b->nposts + (size_t)per_page - 1) / (size_t)per_page);
    if (pages < 1) pages = 1;
  }
  for (long k = 1; k <= pages; k++) {
    val_obj_set(root, str_c("page"), &VAL_UNDEF);
    Val *plist = val_arr(a);
    size_t from = (size_t)(k - 1) * (size_t)per_page;
    size_t to = (size_t)k * (size_t)per_page;
    if (to > b->nposts) to = b->nposts;
    for (size_t i = from; i < to; i++) val_arr_push(plist, b->posts[i]->page_val);
    val_obj_set(root, str_c("posts"), plist);
    Val *pager = val_obj(a);
    val_obj_set(pager, str_c("number"), val_num(a, (double)k));
    val_obj_set(pager, str_c("total"), val_num(a, (double)pages));
    StrBuf u1; sb_init(&u1);
    sb_puts(&u1, b->base); sb_putc(&u1, '/');
    if (k != 1) { char tmp[32]; snprintf(tmp, sizeof tmp, "page/%ld/", k); sb_put(&u1, tmp); }
    Str url_k = sb_to_arena(a, &u1);
    val_obj_set(pager, str_c("url"), val_str(a, url_k));
    Str newer_url = str_c("");
    if (k != 1) {
      StrBuf u2; sb_init(&u2);
      sb_puts(&u2, b->base); sb_putc(&u2, '/');
      if (k - 1 != 1) { char tmp[32]; snprintf(tmp, sizeof tmp, "page/%ld/", k - 1); sb_put(&u2, tmp); }
      newer_url = sb_to_arena(a, &u2);
    }
    val_obj_set(pager, str_c("newer_url"), val_str(a, newer_url));
    Str older_url = str_c("");
    if (k != pages) {
      StrBuf u3; sb_init(&u3);
      sb_puts(&u3, b->base); sb_putc(&u3, '/');
      char tmp[32]; snprintf(tmp, sizeof tmp, "page/%ld/", k + 1); sb_put(&u3, tmp);
      older_url = sb_to_arena(a, &u3);
    }
    val_obj_set(pager, str_c("older_url"), val_str(a, older_url));
    val_obj_set(root, str_c("pager"), pager);
    val_obj_set(root, str_c("tag"), &VAL_UNDEF);
    Str html;
    if (render_tpl(&b->tpl, str_c("index"), &root_frame, &html)) {
      if (k == 1) build_out_add(b, str_c("index.html"), html);
      else {
        StrBuf kk; sb_init(&kk);
        char tmp[32]; snprintf(tmp, sizeof tmp, "page/%ld/index.html", k);
        sb_put(&kk, tmp);
        build_out_add(b, sb_to_arena(a, &kk), html);
      }
    }
  }
  for (size_t ti = 0; ti < b->ntags; ti++) {
    Tag *t = b->tags[ti];
    val_obj_set(root, str_c("page"), &VAL_UNDEF);
    val_obj_set(root, str_c("posts"), &VAL_UNDEF);
    val_obj_set(root, str_c("pager"), &VAL_UNDEF);
    Val *tagv = val_obj(a);
    val_obj_set(tagv, str_c("name"), val_str(a, t->name));
    val_obj_set(tagv, str_c("slug"), val_str(a, t->slug));
    val_obj_set(tagv, str_c("url"), val_str(a, t->url));
    val_obj_set(tagv, str_c("count"), val_num(a, (double)t->nposts));
    Val *tposts = val_arr(a);
    for (size_t i = 0; i < t->nposts; i++) val_arr_push(tposts, t->posts[i]->page_val);
    val_obj_set(tagv, str_c("posts"), tposts);
    val_obj_set(root, str_c("tag"), tagv);
    Str html;
    if (render_tpl(&b->tpl, str_c("tag"), &root_frame, &html)) {
      StrBuf kk; sb_init(&kk);
      sb_put(&kk, "tags/"); sb_puts(&kk, t->slug); sb_put(&kk, "/index.html");
      build_out_add(b, sb_to_arena(a, &kk), html);
    }
  }
  val_obj_set(root, str_c("page"), &VAL_UNDEF);
  val_obj_set(root, str_c("posts"), &VAL_UNDEF);
  val_obj_set(root, str_c("pager"), &VAL_UNDEF);
  val_obj_set(root, str_c("tag"), &VAL_UNDEF);
  Str tags_html;
  if (render_tpl(&b->tpl, str_c("tags"), &root_frame, &tags_html))
    build_out_add(b, str_c("tags/index.html"), tags_html);
  for (size_t fi = 0; fi < b->nstatics; fi++) {
    if (build_out_has(b, b->statics[fi].path)) {
      StrBuf r; sb_init(&r);
      sb_put(&r, "shadowed "); sb_puts(&r, b->statics[fi].path);
      sb_put(&r, ": a built page has its path");
      build_report_push(b, sb_to_arena(a, &r));
    } else build_out_add(b, b->statics[fi].path, b->statics[fi].content);
  }

  /* 8. report */
  size_t nlines = b->nreport + b->nbroken;
  Str *lines = malloc((nlines ? nlines : 1) * sizeof(Str));
  size_t li = 0;
  for (size_t i = 0; i < b->nreport; i++) lines[li++] = b->report[i];
  for (size_t i = 0; i < b->nbroken; i++) lines[li++] = b->broken[i];
  isort(lines, nlines, sizeof(Str), cmp_strp);
  size_t npages = 0;
  for (size_t i = 0; i < b->nsources; i++) if (!b->sources[i]->post) npages++;
  StrBuf rep; sb_init(&rep);
  for (size_t i = 0; i < nlines; i++) { sb_puts(&rep, lines[i]); sb_putc(&rep, '\n'); }
  free(lines);
  char tmp[128];
  int tn = snprintf(tmp, sizeof tmp, "built %zu pages, %zu posts, %zu tags and %zu static files",
                    npages, b->nposts, b->ntags, b->nstatics);
  sb_putn(&rep, tmp, (size_t)tn);
  sb_putc(&rep, '\n');
  build_out_add(b, str_c("build-report.txt"), sb_to_arena(a, &rep));
}

/* ------------------------------------------------------------------- main */
static char *read_entire_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 1 << 20, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
    size_t r = fread(buf + len, 1, cap - len, f);
    len += r;
    if (r == 0) break;
  }
  fclose(f);
  *out_len = len;
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: sitegen <inputDir> <outputDir>\n");
    return 2;
  }
  char inpath[4096], outpath[4096];
  snprintf(inpath, sizeof inpath, "%s/cases.json", argv[1]);
  snprintf(outpath, sizeof outpath, "%s/answers.json", argv[2]);
  size_t file_len = 0;
  char *file_buf = read_entire_file(inpath, &file_len);
  if (!file_buf) { fprintf(stderr, "sitegen: cannot read %s\n", inpath); return 1; }
  Arena input_arena;
  arena_init(&input_arena);
  int err = 0;
  JVal *cases = json_parse(&input_arena, file_buf, file_len, &err);
  if (err || !cases || cases->t != J_ARR) {
    fprintf(stderr, "sitegen: cases.json did not parse\n");
    return 1;
  }
  StrBuf out; sb_init(&out);
  sb_putc(&out, '[');
  for (size_t ci = 0; ci < cases->nitems; ci++) {
    JVal *input = jobj_get(cases->items[ci], "input");
    Arena case_arena;
    arena_init(&case_arena);
    Build b;
    memset(&b, 0, sizeof b);
    b.arena = &case_arena;
    build_case(&b, input);
    if (ci) sb_putc(&out, ',');
    sb_putc(&out, '{');
    for (size_t i = 0; i < b.nout; i++) {
      if (i) sb_putc(&out, ',');
      json_escape_str(&out, b.out_keys[i]);
      sb_putc(&out, ':');
      json_escape_str(&out, b.out_vals[i]);
    }
    sb_putc(&out, '}');
    arena_reset(&case_arena);
    /* free build's malloc'd arrays */
    free(b.report); free(b.broken);
    free(b.out_keys); free(b.out_vals);
    free(b.sources); free(b.statics); free(b.tags); free(b.posts);
    for (size_t i = 0; i < b.tpl.ntmpls; i++) free(b.tpl.deps[i]);
    free(b.tpl.deps); free(b.tpl.ndeps);
    for (size_t i = 0; i < b.tpl.ncache; i++) free(b.tpl.cache[i].kv);
    free(b.tpl.cache);
    free(b.tpl.tmpls); free(b.slugcache.e);
    for (size_t i = 0; i < b.ndcs; i++) free(b.dcs[i].ents);
    free(b.dcs);
  }
  sb_putc(&out, ']');
  mkdir(argv[2], 0777);
  FILE *f = fopen(outpath, "wb");
  if (!f) { fprintf(stderr, "sitegen: cannot write %s\n", outpath); return 1; }
  fwrite(out.buf, 1, out.len, f);
  fclose(f);
  return 0;
}

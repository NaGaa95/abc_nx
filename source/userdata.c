/* userdata.c -- persistent key/value store backing UserValue.* (see userdata.h)
 *
 * File format ("userdata.txt"), one entry per line:
 *   <type> <key> <value>
 * where <type> is one of i (int), b (bool), l (long), f (float), s (string).
 * For strings, <value> is the remainder of the line (may contain spaces).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userdata.h"

#define UD_FILE     "userdata.txt"
#define UD_MAX      512
#define UD_KEY_LEN  128
#define UD_STR_LEN  256

typedef struct {
  char  key[UD_KEY_LEN];
  char  type;                 // i/b/l/f/s
  int64_t ival;               // int/bool/long
  double  fval;               // float
  char  sval[UD_STR_LEN];     // string
} Entry;

static Entry g_ud[UD_MAX];
static int   g_ud_count = 0;
static int   g_ud_loaded = 0;

static Entry *ud_find(const char *key) {
  for (int i = 0; i < g_ud_count; i++)
    if (strcmp(g_ud[i].key, key) == 0)
      return &g_ud[i];
  return NULL;
}

static Entry *ud_slot(const char *key) {
  Entry *e = ud_find(key);
  if (e)
    return e;
  if (g_ud_count >= UD_MAX)
    return NULL;
  e = &g_ud[g_ud_count++];
  memset(e, 0, sizeof(*e));
  snprintf(e->key, sizeof(e->key), "%.*s", (int)sizeof(e->key) - 1, key);
  return e;
}

static void ud_save_file(void) {
  FILE *f = fopen(UD_FILE, "w");
  if (!f)
    return;
  for (int i = 0; i < g_ud_count; i++) {
    Entry *e = &g_ud[i];
    switch (e->type) {
      case 'i': fprintf(f, "i %s %ld\n", e->key, (long)e->ival); break;
      case 'b': fprintf(f, "b %s %d\n", e->key, (int)(e->ival != 0)); break;
      case 'l': fprintf(f, "l %s %lld\n", e->key, (long long)e->ival); break;
      case 'f': fprintf(f, "f %s %.9g\n", e->key, e->fval); break;
      case 's': fprintf(f, "s %s %s\n", e->key, e->sval); break;
      default: break;
    }
  }
  fclose(f);
}

void ud_load(void) {
  if (g_ud_loaded)
    return;
  g_ud_loaded = 1;

  FILE *f = fopen(UD_FILE, "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    // strip trailing newline
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n < 3 || line[1] != ' ')
      continue;
    char type = line[0];
    char *key = line + 2;
    char *sp = strchr(key, ' ');
    if (!sp)
      continue;
    *sp = 0;
    char *val = sp + 1;
    Entry *e = ud_slot(key);
    if (!e)
      break;
    e->type = type;
    switch (type) {
      case 'i': case 'b': case 'l': e->ival = strtoll(val, NULL, 10); break;
      case 'f': e->fval = strtod(val, NULL); break;
      case 's': snprintf(e->sval, sizeof(e->sval), "%s", val); break;
      default: e->type = 0; break;
    }
  }
  fclose(f);
}

int ud_has(const char *key) {
  return ud_find(key) != NULL;
}

int32_t ud_get_int(const char *key, int32_t def) {
  Entry *e = ud_find(key);
  return e ? (int32_t)e->ival : def;
}
int ud_get_bool(const char *key, int def) {
  Entry *e = ud_find(key);
  return e ? (e->ival != 0) : def;
}
int64_t ud_get_long(const char *key, int64_t def) {
  Entry *e = ud_find(key);
  return e ? e->ival : def;
}
float ud_get_float(const char *key, float def) {
  Entry *e = ud_find(key);
  return e ? (float)e->fval : def;
}
const char *ud_get_string(const char *key, const char *def) {
  Entry *e = ud_find(key);
  return e ? e->sval : def;
}

void ud_set_int(const char *key, int32_t val) {
  Entry *e = ud_slot(key); if (!e) return;
  e->type = 'i'; e->ival = val; ud_save_file();
}
void ud_set_bool(const char *key, int val) {
  Entry *e = ud_slot(key); if (!e) return;
  e->type = 'b'; e->ival = (val != 0); ud_save_file();
}
void ud_set_long(const char *key, int64_t val) {
  Entry *e = ud_slot(key); if (!e) return;
  e->type = 'l'; e->ival = val; ud_save_file();
}
void ud_set_float(const char *key, float val) {
  Entry *e = ud_slot(key); if (!e) return;
  e->type = 'f'; e->fval = val; ud_save_file();
}
void ud_set_string(const char *key, const char *val) {
  Entry *e = ud_slot(key); if (!e) return;
  e->type = 's'; snprintf(e->sval, sizeof(e->sval), "%s", val ? val : ""); ud_save_file();
}

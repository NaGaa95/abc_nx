/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "util.h"
#include "config.h"

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

int debugPrintf(char *text, ...) {
#ifdef DEBUG_LOG
  // serialize across threads; keep one FILE open and fflush per line (open/
  // close-per-line is too slow, fflush still makes hangs visible in the log)
  static Mutex log_mtx; // zero-initialized = unlocked
  static FILE *f = NULL;
  static int tried = 0;
  va_list list;

  mutexLock(&log_mtx);
  if (!f && !tried) { tried = 1; f = fopen(LOG_NAME, "a"); }
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fflush(f);
  }
  mutexUnlock(&log_mtx);
#endif
  return 0;
}

// FastLoad = CPU 1785 MHz but GPU throttled to 76 MHz, so it must be OFF for
// anything that renders. Idempotent + thread-safe (called from log threads).
static Mutex g_boost_lock;
static int   g_boost_on = -1;

void cpu_boost(int on) {
  on = on ? 1 : 0;
  mutexLock(&g_boost_lock);
  if (g_boost_on != on) {
    appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
    g_boost_on = on;
    debugPrintf("cpu_boost: %s\n", on ? "FastLoad" : "Normal");
  }
  mutexUnlock(&g_boost_lock);
}

// Toggle the boost off the engine's state logs (all flow through
// __android_log_print). Boost only during asset loads; Normal for rendering.
// UPDATE_LEVEL_STATE = safety net so gameplay is never left throttled.
void boost_scan_log(const char *msg) {
  if (!msg) return;
  if (strstr(msg, "LOADING TOOK") ||
      strstr(msg, "CINEMATIC_STATE") ||
      strstr(msg, "UPDATE_LEVEL_STATE")) { cpu_boost(0); return; }
  if (strstr(msg, "enter INITIALIZING_STATE")) cpu_boost(1);
}

// The engine opens its data archive by the path getObbPath() returns. Locate the
// Android expansion file in the cwd -- the raw .obb IS the pak archive, so users
// drop it in as-is. Prefers main*.obb; result is cached.
const char *find_obb(void) {
  static char found[256];
  static int done = 0;
  if (done)
    return found[0] ? found : NULL;
  done = 1;

  DIR *d = opendir(".");
  if (d) {
    struct dirent *e;
    char any[256] = { 0 };
    while ((e = readdir(d)) != NULL) {
      const char *ext = strrchr(e->d_name, '.');
      if (!ext || strcasecmp(ext, ".obb") != 0)
        continue;
      if (strncmp(e->d_name, "main", 4) == 0) { // prefer the main expansion file
        snprintf(found, sizeof(found), "%s", e->d_name);
        break;
      }
      if (!any[0])
        snprintf(any, sizeof(any), "%s", e->d_name);
    }
    if (!found[0] && any[0])
      snprintf(found, sizeof(found), "%s", any);
    closedir(d);
  }
  return found[0] ? found : NULL;
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }

/* config.h -- global configuration and config file handling
 *
 * After Burner Climax (Switch port).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// MB reserved for the .so load region: libacb.so (~18.5 MB) + libfmod.so
// (~1.4 MB) + libfmodstudio.so (~1.1 MB) segments, each 2 MB-aligned.
#define MEMORY_SO_MB 64

// After Burner Climax ships the modern split FMOD (low-level + studio).
#define SO_NAME             "libacb.so"
#define FMOD_SO_NAME        "libfmod.so"
#define FMODSTUDIO_SO_NAME  "libfmodstudio.so"
#define CONFIG_NAME  "config.txt"
#define LOG_NAME     "debug.log"

// Uncomment to write debug.log (per-line SD writes; slows boot).
// #define DEBUG_LOG 1

// actual screen size in use right now
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif

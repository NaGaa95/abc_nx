/* userdata.h -- persistent key/value store backing UserValue.*
 *
 * On Android, After Burner Climax persists settings + progress through
 * com.sega.f2fextension.UserValue (SharedPreferences) via JNI. We back that
 * with a simple text file ("userdata.txt") in the game folder so saves survive
 * across runs. Keys are identifiers (no whitespace); values are typed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __USERDATA_H__
#define __USERDATA_H__

#include <stdint.h>

void ud_load(void);   // read userdata.txt (idempotent; call once at startup)

int  ud_has(const char *key);

int32_t ud_get_int(const char *key, int32_t def);
int     ud_get_bool(const char *key, int def);
int64_t ud_get_long(const char *key, int64_t def);
float   ud_get_float(const char *key, float def);
// returns a heap copy owned by the store's jstring cache; caller must not free
const char *ud_get_string(const char *key, const char *def);

void ud_set_int(const char *key, int32_t val);
void ud_set_bool(const char *key, int val);
void ud_set_long(const char *key, int64_t val);
void ud_set_float(const char *key, float val);
void ud_set_string(const char *key, const char *val);

#endif

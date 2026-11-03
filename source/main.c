/* main.c -- After Burner Climax (Switch port) entry point
 *
 * Loads libacb.so (the mojito/F2F NDK NativeActivity engine) together with the
 * split FMOD runtime (libfmod.so + libfmodstudio.so), provides a minimal
 * Android environment, seeds the engine's data-archive path (the OBB), and
 * drives the Android lifecycle + gamepad input while the engine renders on its
 * own thread.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "saltynx.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "android_host.h"
#include "jni_fake.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module fmod_mod;       // libfmod.so       (low-level FMOD, drives OpenSL)
so_module fmodstudio_mod; // libfmodstudio.so (FMOD Studio, depends on libfmod)
so_module game_mod;       // libacb.so        (the engine)

static size_t g_mem_total_mb = 0, g_mem_newlib_mb = 0, g_mem_so_mb = 0;

// bionic TLS for the main thread: engine reads its stack-guard cookie from
// TPIDR_EL0+40, which the Switch leaves unset.
static uint8_t main_fake_tls[0x100];

// ---------------------------------------------------------------------------
// heap split: keep the newlib heap (malloc + mesa GPU bos) separate from the
// .so load region (mapped engine + FMOD segments).
// ---------------------------------------------------------------------------
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t so_region = (size_t)MEMORY_SO_MB * 1024 * 1024;
  if (so_region > size / 4)
    so_region = size / 4;
  fake_heap_size  = size - so_region;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_mem_total_mb  = size >> 20;
  g_mem_newlib_mb = fake_heap_size >> 20;
  g_mem_so_mb     = so_region >> 20;
}

// ---------------------------------------------------------------------------
// data archive (OBB) path. The engine opens exactly one zip -- the OBB -- via
// unzOpen(_apk_path) and requires the path to contain the literal "/obb/"
// (a gate) and to be fopen-able; entries resolve by their raw names. So we keep
// the OBB under an "obb/" subfolder of the game dir and hand the engine an
// absolute path to it. If the user dropped the OBB directly in the game folder,
// migrate it into obb/ (a same-filesystem rename is instant).
// ---------------------------------------------------------------------------
static char g_obb_path[512];   // absolute path passed to nativeSetApkPath
static char g_obb_name[256];   // bare filename (for setObbFileName)
static char g_obb_rel[300];    // "obb/<filename>" relative to cwd (GetDataArchiveFilePath)

// The engine's APPLICATION::GetArchiveFilePath asks Java (AfterBurnerClimax.
// GetDataArchiveFilePath) for the data-archive path and fopen()s it directly.
// A cwd-relative "obb/<name>" resolves correctly whether the engine uses it
// verbatim or prefixes it with "./", and avoids any sdmc: absolute-path fixups.
const char *abc_data_archive_path(void) { return g_obb_rel; }

static const char *find_obb_in(const char *dir) {
  static char found[256];
  DIR *d = opendir(dir);
  if (!d)
    return NULL;
  found[0] = 0;
  char any[256] = {0};
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *ext = strrchr(e->d_name, '.');
    if (!ext || strcasecmp(ext, ".obb") != 0)
      continue;
    if (strncmp(e->d_name, "main", 4) == 0) { snprintf(found, sizeof(found), "%s", e->d_name); break; }
    if (!any[0]) snprintf(any, sizeof(any), "%s", e->d_name);
  }
  if (!found[0] && any[0]) snprintf(found, sizeof(found), "%s", any);
  closedir(d);
  return found[0] ? found : NULL;
}

// Locate the OBB and produce an absolute path under obb/ containing "/obb/".
// Returns 1 on success (g_obb_path/g_obb_name populated), 0 if none found.
static int locate_obb(void) {
  char cwd[256];
  if (!getcwd(cwd, sizeof(cwd)))
    snprintf(cwd, sizeof(cwd), ".");
  size_t cl = strlen(cwd);
  if (cl && (cwd[cl - 1] == '/' || cwd[cl - 1] == '\\')) cwd[cl - 1] = 0;

  // 1) already under obb/
  const char *n = find_obb_in("obb");
  if (n) {
    snprintf(g_obb_name, sizeof(g_obb_name), "%s", n);
    snprintf(g_obb_path, sizeof(g_obb_path), "%s/obb/%s", cwd, n);
    snprintf(g_obb_rel, sizeof(g_obb_rel), "obb/%s", n);
    return 1;
  }
  // 2) directly in the game folder -> migrate into obb/ so the path gets "/obb/"
  n = find_obb_in(".");
  if (n) {
    snprintf(g_obb_name, sizeof(g_obb_name), "%s", n);
    snprintf(g_obb_rel, sizeof(g_obb_rel), "obb/%s", n);
    mkdir("obb", 0777);
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/%s", cwd, n);
    snprintf(dst, sizeof(dst), "%s/obb/%s", cwd, n);
    if (rename(src, dst) == 0) {
      snprintf(g_obb_path, sizeof(g_obb_path), "%s", dst);
      return 1;
    }
    // rename failed: fall back to the in-place path (lacks "/obb/", flagged below)
    snprintf(g_obb_path, sizeof(g_obb_path), "%s", src);
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// startup checks
// ---------------------------------------------------------------------------
static void check_data(void) {
  const char *files[] = { SO_NAME, FMOD_SO_NAME, FMODSTUDIO_SO_NAME };
  struct stat st;
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); ++i) {
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nCheck your data files in the game folder.", files[i]);
  }
  if (!locate_obb())
    fatal_error("Could not find the game data.\nPut the .obb file in the\ngame folder (or its obb/ subfolder).");
  if (!strstr(g_obb_path, "/obb/"))
    fatal_error("Could not place the .obb under an\nobb/ folder. Create an obb/ subfolder\nand move the .obb there.");
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

// Horizon traps EL0 reads of CNTVCT_EL0 (the virtual counter); only CNTPCT_EL0
// (physical) is permitted from userspace. BoringSSL's _armv7_tick -- reached
// from the OPENSSL_cpuid_setup static constructor and the RNG -- executes
// `mrs x0, cntvct_el0` and faults (there is no working SIGILL handler to catch
// it). Rewrite every such read to `mrs x0, cntpct_el0` while the module is still
// writable, before so_finalize maps it as code. (libacb has exactly one.)
static void patch_cntvct(so_module *mod) {
  const uint32_t CNTVCT = 0xd53be040; // mrs x0, cntvct_el0
  const uint32_t CNTPCT = 0xd53be020; // mrs x0, cntpct_el0
  int n = 0;
  for (int s = 0; s < mod->phnum; s++) {
    const Elf64_Phdr *p = &mod->phdr[s];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    uint32_t *code = (uint32_t *)((uint8_t *)mod->load_base + p->p_vaddr);
    const size_t words = p->p_memsz / 4;
    for (size_t i = 0; i < words; i++)
      if (code[i] == CNTVCT) { code[i] = CNTPCT; n++; }
  }
  debugPrintf("patch: rewrote %d cntvct_el0 -> cntpct_el0\n", n);
}

// Force the SEGA "F2F" age-gate / GDPR consent predicates to "granted". The gate
// is persistence-driven (not dialog-driven): with no Java consent UI on the
// Switch, F2FExtension::runUpdateGDPRRule parks forever while haveRemoveAgeGate()
// returns 0. These predicates ("old enough / gate removed / form finished /
// consent granted") gate the flow; returning 1 from all of them makes it
// complete and proceed to the title. Addresses are for this libacb build.
static void patch_ret1(so_module *mod, uintptr_t vaddr) {
  uint32_t *p = (uint32_t *)((uint8_t *)mod->load_base + vaddr);
  debugPrintf("patch: %llx orig=%08x -> ret 1\n", (unsigned long long)vaddr, p[0]);
  p[0] = 0x52800020; // movz w0, #1
  p[1] = 0xd65f03c0; // ret
}

static void patch_agegate(so_module *mod) {
  patch_ret1(mod, 0xae54b0); // haveRemoveAgeGate  -> 1 (releases the park loop)
  patch_ret1(mod, 0xae4770); // isEnoughtAge       -> 1
  patch_ret1(mod, 0xae513c); // getGDPRFormFinish  -> 1
  patch_ret1(mod, 0xaef5c4); // haveGDPRFormFinish -> 1
  patch_ret1(mod, 0xaef718); // isConsentCountry   -> 1
  patch_ret1(mod, 0xaefd14); // haveUserConsent    -> 1
  patch_ret1(mod, 0xaefc18); // checkConsentGDPR   -> 1
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// Android gamepad key codes (NDK android/keycodes.h) and motion axes, matching
// what libacb's INPUT_MOGA_JOYPAD / INPUT_SYSTEM read back.
// ---------------------------------------------------------------------------
#define AKEYCODE_BACK          4
#define AKEYCODE_BUTTON_A      96   // machine gun / fire / confirm
#define AKEYCODE_BUTTON_B      97   // secondary / cancel
#define AKEYCODE_BUTTON_X      99   // climax special
#define AKEYCODE_BUTTON_Y      100  // action
#define AKEYCODE_BUTTON_L1     102  // throttle / roll
#define AKEYCODE_BUTTON_R1     103  // throttle / roll
#define AKEYCODE_BUTTON_START  108  // start / pause
#define AKEYCODE_BUTTON_SELECT 109  // select

// AMotionEvent axis ids the engine reads via AMotionEvent_getAxisValue
#define AXIS_X      0
#define AXIS_Y      1
#define AXIS_Z      11
#define AXIS_RZ     14
#define AXIS_HAT_X  15
#define AXIS_HAT_Y  16

typedef struct { u64 hid; int keycode; } PadKeyMap;

// Nintendo face layout: A = fire, X = climax, B/Y = the other actions.
static const PadKeyMap pad_face[] = {
  { HidNpadButton_A, AKEYCODE_BUTTON_A },   // A (right)  -> machine gun / fire
  { HidNpadButton_B, AKEYCODE_BUTTON_B },   // B (bottom) -> secondary
  { HidNpadButton_X, AKEYCODE_BUTTON_X },   // X (top)    -> climax special
  { HidNpadButton_Y, AKEYCODE_BUTTON_Y },   // Y (left)   -> action
};
static const PadKeyMap pad_keys[] = {
  { HidNpadButton_L,  AKEYCODE_BUTTON_L1 }, // L  -> throttle / roll
  { HidNpadButton_R,  AKEYCODE_BUTTON_R1 }, // R  -> throttle / roll
  { HidNpadButton_ZR, AKEYCODE_BUTTON_A },  // ZR -> alt fire (trigger)
  { HidNpadButton_ZL, AKEYCODE_BUTTON_X },  // ZL -> alt climax
  { HidNpadButton_Plus,  AKEYCODE_BUTTON_START }, // Start -> pause
  { HidNpadButton_Minus, AKEYCODE_BACK },         // Select -> back
};

static PadState pad;
static u64 pad_prev = 0;

static void send_key(int keycode, int down, int source) {
  AInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.source = source;
  ev.key_action = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
  ev.key_code = keycode;
  android_host_post_input(&ev);
}

static void send_pad_keys(const PadKeyMap *map, unsigned n, u64 down, u64 changed) {
  for (unsigned i = 0; i < n; i++) {
    if (!(changed & map[i].hid))
      continue;
    send_key(map[i].keycode, (down & map[i].hid) != 0, AINPUT_SOURCE_GAMEPAD);
  }
}

// Emit a joystick MOTION event carrying all analog axes the engine samples.
// The engine reads axis[] via AMotionEvent_getAxisValue (deadzone |v| > 0.15).
static void post_axes(float lx, float ly, float rx, float ry, float hx, float hy) {
  AInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.motion_action = AMOTION_EVENT_ACTION_MOVE;
  ev.pointer_count = 1;
  ev.axis[AXIS_X]     = lx;
  ev.axis[AXIS_Y]     = ly;
  ev.axis[AXIS_Z]     = rx;
  ev.axis[AXIS_RZ]    = ry;
  ev.axis[AXIS_HAT_X] = hx;
  ev.axis[AXIS_HAT_Y] = hy;
  // mirror the primary stick into getX/getY for any code that reads those
  ev.x[0] = lx;
  ev.y[0] = ly;
  android_host_post_input(&ev);
}

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 changed = down ^ pad_prev;

  send_pad_keys(pad_face, sizeof(pad_face) / sizeof(*pad_face), down, changed);
  send_pad_keys(pad_keys, sizeof(pad_keys) / sizeof(*pad_keys), down, changed);

  // analog sticks (normalized to [-1, 1]; Android AXIS_Y points down)
  const float scale = 1.0f / 32767.0f;
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  float lx =  (float)ls.x * scale, ly = -(float)ls.y * scale;
  float rx =  (float)rs.x * scale, ry = -(float)rs.y * scale;

  // d-pad -> HAT axes (the engine lets these override the left-stick slot)
  float hx = (float)((down & HidNpadButton_Right) != 0) - (float)((down & HidNpadButton_Left) != 0);
  float hy = (float)((down & HidNpadButton_Down)  != 0) - (float)((down & HidNpadButton_Up)   != 0);

  // resend only when an axis moves enough to matter (avoids flooding the queue)
  static float p[6] = {0, 0, 0, 0, 0, 0};
  const float cur[6] = { lx, ly, rx, ry, hx, hy };
  int changed_axis = 0;
  for (int i = 0; i < 6; i++)
    if (fabsf(cur[i] - p[i]) > 0.03f) { changed_axis = 1; break; }
  if (changed_axis) {
    memcpy(p, cur, sizeof(p));
    post_axes(lx, ly, rx, ry, hx, hy);
  }

  pad_prev = down;
}

// touch (handheld): translate panel fingers into TOUCHSCREEN motion events for
// menu navigation and the on-screen virtual controls.
#define MAX_TOUCH 8
typedef struct { int active; u32 fid; float x, y; } TouchSlot;
static TouchSlot touch_prev[MAX_TOUCH];

static void post_touch(int action, int slot) {
  AInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.source = AINPUT_SOURCE_TOUCHSCREEN;
  ev.motion_action = action;
  int n = 0;
  for (int i = 0; i < MAX_TOUCH; i++) {
    if (touch_prev[i].active) {
      ev.pointer_id[n] = i;
      ev.x[n] = touch_prev[i].x;
      ev.y[n] = touch_prev[i].y;
      n++;
    }
  }
  if (action != AMOTION_EVENT_ACTION_UP && n == 0)
    ev.pointer_count = 1;
  else
    ev.pointer_count = n ? n : 1;
  (void)slot;
  android_host_post_input(&ev);
}

static void update_touch(void) {
  HidTouchScreenState st = {0};
  if (!hidGetTouchScreenStates(&st, 1))
    return;
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;
  int seen[MAX_TOUCH] = {0};

  for (int i = 0; i < st.count && i < MAX_TOUCH; i++) {
    const HidTouchState *t = &st.touches[i];
    int slot = -1;
    for (int s = 0; s < MAX_TOUCH; s++)
      if (touch_prev[s].active && touch_prev[s].fid == t->finger_id) { slot = s; break; }
    int is_new = 0;
    if (slot < 0) {
      for (int s = 0; s < MAX_TOUCH; s++)
        if (!touch_prev[s].active) { slot = s; break; }
      if (slot < 0) continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].fid = t->finger_id;
      is_new = 1;
    }
    const float nx = (float)t->x * sx;
    const float ny = (float)t->y * sy;
    const int moved = fabsf(nx - touch_prev[slot].x) > 2.0f ||
                      fabsf(ny - touch_prev[slot].y) > 2.0f;
    touch_prev[slot].x = nx;
    touch_prev[slot].y = ny;
    seen[slot] = 1;
    if (is_new)
      post_touch(AMOTION_EVENT_ACTION_DOWN, slot);
    else if (moved)
      post_touch(AMOTION_EVENT_ACTION_MOVE, slot);
  }
  for (int s = 0; s < MAX_TOUCH; s++) {
    if (touch_prev[s].active && !seen[s]) {
      post_touch(AMOTION_EVENT_ACTION_UP, s);
      touch_prev[s].active = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// fake ANativeActivity + lifecycle
// ---------------------------------------------------------------------------
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks; // zeroed; populated by the glue
static AAssetManager *g_asset_mgr = (AAssetManager *)1;
static ANativeWindow *g_window;

static uint8_t lifecycle_fake_tls[0x100];

static void *lifecycle_thread(void *arg) {
  (void)arg;
  memset(lifecycle_fake_tls, 0, sizeof(lifecycle_fake_tls));
  armSetTlsRw(lifecycle_fake_tls);

  g_window = android_host_get_window();
  if (g_callbacks.onStart) g_callbacks.onStart(&g_activity);
  if (g_callbacks.onResume) g_callbacks.onResume(&g_activity);
  debugPrintf("onNativeWindowCreated\n");
  if (g_callbacks.onNativeWindowCreated) g_callbacks.onNativeWindowCreated(&g_activity, g_window);
  if (g_callbacks.onWindowFocusChanged) g_callbacks.onWindowFocusChanged(&g_activity, 1);
  debugPrintf("onInputQueueCreated\n");
  android_host_create_input_queue(&g_activity);
  debugPrintf("lifecycle thread done\n");
  return NULL;
}

// ---------------------------------------------------------------------------
// bootstrap natives (exported by libacb, resolved before so_finalize). The
// Android Java layer normally calls these; we replicate them to seed the
// engine's data path + display before android_main touches FileUtils.
// ---------------------------------------------------------------------------
typedef void (*fn_set_str)(void *env, void *clazz, void *jstr);
typedef void (*fn_set_obb)(void *env, void *clazz, void *jstr, int mode);
typedef void (*fn_set_wh)(void *env, void *clazz, int w, int h);
typedef void (*fn_set_scale)(void *env, void *clazz, float a, float b);
typedef void (*fn_dev)(void *env, void *clazz, int id);

static fn_set_str   p_nativeSetApkPath;
static fn_set_obb   p_setObbFileName;
static fn_set_wh    p_setScreenSize;
static fn_set_scale p_setScreenScaleDesity;
static fn_dev       p_deviceAdded;

// deviceAdded() flags a controller as present, but it writes through
//   obj = *(void**)( *(void**)(libacb + 0x1204628) );  obj->byte[16] = 1;
// and obj is NULL until the engine finishes building its input subsystem on the
// android_main thread. Calling it too early faults, so we poll that same deref
// chain from the input loop and fire exactly once, when the object exists.
#define ACB_INPUTMGR_SLOT 0x1204628
static int g_controller_signaled = 0;

static void try_signal_controller(void) {
  if (g_controller_signaled || !p_deviceAdded)
    return;
  void **pmgr = (void **)((uintptr_t)game_mod.load_virtbase + ACB_INPUTMGR_SLOT);
  void *mgr = *pmgr;
  if (!mgr)
    return;                       // manager singleton not constructed yet
  void *obj = *(void **)mgr;      // its controller-state object
  if (!obj)
    return;                       // not allocated yet -> deviceAdded would fault
  p_deviceAdded(fake_env, jni_make_object("AfterBurnerClimax"), 0);
  g_controller_signaled = 1;
  debugPrintf("deviceAdded: controller flagged present\n");
}

int main(void) {
  saltynx_start();
  atexit(saltynx_stop);
  // FastLoad for the boot load; boost_scan_log() (util.c) drops to Normal for
  // rendering. See util.c.
  cpu_boost(1);

#ifdef DEBUG_LOG
  FILE *lf = fopen(LOG_NAME, "w");
  if (lf) fclose(lf);
#endif
  debugPrintf("=== abc_nx boot ===\n");

  read_config(CONFIG_NAME);
  write_config(CONFIG_NAME);
  debugPrintf("config read; checking syscalls + data\n");

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);
  debugPrintf("obb archive: %s\n", g_obb_path);

  debugPrintf("mem: total=%zu MB | newlib=%zu MB | .so region=%zu MB\n",
              g_mem_total_mb, g_mem_newlib_mb, g_mem_so_mb);

  // Load FMOD low-level first, then FMOD Studio (depends on it), then the game.
  // All three are resolved before any is finalized (finalize makes a module's
  // symbols unreadable), so cross-module imports bind correctly.
  if (so_load(&fmod_mod, FMOD_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", FMOD_SO_NAME);
  so_relocate(&fmod_mod);
  so_resolve(&fmod_mod, dynlib_functions, dynlib_numfunctions, 1);
  size_t off = ALIGN_MEM(fmod_mod.load_size, 0x200000);

  if (so_load(&fmodstudio_mod, FMODSTUDIO_SO_NAME, (char *)heap_so_base + off, heap_so_limit - off) < 0)
    fatal_error("Could not load\n%s.", FMODSTUDIO_SO_NAME);
  so_relocate(&fmodstudio_mod);
  so_resolve(&fmodstudio_mod, dynlib_functions, dynlib_numfunctions, 1);
  off += ALIGN_MEM(fmodstudio_mod.load_size, 0x200000);

  if (so_load(&game_mod, SO_NAME, (char *)heap_so_base + off, heap_so_limit - off) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
  update_imports();
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  // Resolve entry points + bootstrap natives before so_finalize remaps the load
  // region as code memory (after which .dynsym/.dynstr are inaccessible).
  int (*JNI_OnLoad)(void *vm, void *reserved) =
      (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  // libfmod's own JNI_OnLoad caches the JavaVM FMOD's Android layer needs.
  // Resolve now (fmod_mod symbols readable until so_finalize).
  int (*fmod_JNI_OnLoad)(void *vm, void *reserved) =
      (void *)so_try_find_addr_rx(&fmod_mod, "JNI_OnLoad");
  // reals for the OpenSL-output-forcing hook in imports.c
  extern int (*g_real_fmod_System_init)(void *, int, unsigned, void *);
  extern int (*g_real_fmod_System_setOutput)(void *, int);
  g_real_fmod_System_init      = (void *)so_try_find_addr_rx(&fmod_mod, "_ZN4FMOD6System4initEijPv");
  g_real_fmod_System_setOutput = (void *)so_try_find_addr_rx(&fmod_mod, "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
  void (*ANativeActivity_onCreate)(ANativeActivity *, void *, size_t) =
      (void *)so_find_addr_rx(&game_mod, "ANativeActivity_onCreate");
  p_nativeSetApkPath     = (fn_set_str)so_try_find_addr_rx(&game_mod, "Java_com_sega_f2fextension_F2FAndroidJNI_nativeSetApkPath");
  p_setObbFileName       = (fn_set_obb)so_try_find_addr_rx(&game_mod, "Java_com_sega_f2fextension_F2FAndroidJNI_setObbFileName");
  p_setScreenSize        = (fn_set_wh)so_try_find_addr_rx(&game_mod, "Java_com_sega_f2fextension_F2FAndroidJNI_setScreenSize");
  p_setScreenScaleDesity = (fn_set_scale)so_try_find_addr_rx(&game_mod, "Java_com_sega_f2fextension_F2FAndroidJNI_setScreenScaleDesity");
  p_deviceAdded          = (fn_dev)so_try_find_addr_rx(&game_mod, "Java_com_sega_afterburnerclimax_AfterBurnerClimax_deviceAdded");

  memset(main_fake_tls, 0, sizeof(main_fake_tls));
  armSetTlsRw(main_fake_tls);

  // Finalize + init each module in dependency order.
  so_finalize(&fmod_mod);
  so_flush_caches(&fmod_mod);
  so_execute_init_array(&fmod_mod);
  so_free_temp(&fmod_mod);

  so_finalize(&fmodstudio_mod);
  so_flush_caches(&fmodstudio_mod);
  so_execute_init_array(&fmodstudio_mod);
  so_free_temp(&fmodstudio_mod);

  patch_cntvct(&game_mod);   // must precede finalize (needs the RW code copy)
  patch_agegate(&game_mod);  // skip the SEGA F2F age-gate/GDPR consent wall
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  debugPrintf("so_finalize OK; running init_array\n");
  so_execute_init_array(&game_mod);
  so_free_temp(&game_mod);
  debugPrintf("init_array OK\n");

  android_host_init();
  jni_init();

  // run both JNI_OnLoads (as the Java runtime would). FMOD's is required, or its
  // Android output init aborts with no JavaVM and every sound event fails.
  if (fmod_JNI_OnLoad) {
    debugPrintf("calling libfmod JNI_OnLoad\n");
    fmod_JNI_OnLoad(fake_vm, NULL);
  }
  if (JNI_OnLoad) {
    debugPrintf("calling JNI_OnLoad\n");
    JNI_OnLoad(fake_vm, NULL);
  }

  // Build the fake activity. Data lives in the game folder (cwd); the OBB path
  // is handed to the engine via nativeSetApkPath below.
  memset(&g_activity, 0, sizeof(g_activity));
  memset(&g_callbacks, 0, sizeof(g_callbacks));
  g_activity.callbacks = &g_callbacks;
  g_activity.vm = fake_vm;
  g_activity.env = fake_env;
  g_activity.clazz = jni_make_string("AfterBurnerClimax");
  g_activity.internalDataPath = ".";
  g_activity.externalDataPath = ".";
  g_activity.obbPath = g_obb_path;
  g_activity.sdkVersion = 30;
  g_activity.assetManager = g_asset_mgr;

  // Seed the engine's data-archive path + display BEFORE onCreate spawns
  // android_main (which reads _apk_path during FileUtils init). The engine
  // fopen()s g_obb_path (which contains "/obb/") and reads it as its zip VFS.
  void *fake_clazz = jni_make_object("F2FAndroidJNI");
  if (p_nativeSetApkPath) {
    debugPrintf("nativeSetApkPath(%s)\n", g_obb_path);
    p_nativeSetApkPath(fake_env, fake_clazz, jni_make_string(g_obb_path));
  } else {
    fatal_error("libacb.so is missing nativeSetApkPath;\nincompatible game version.");
  }
  if (p_setObbFileName)
    p_setObbFileName(fake_env, fake_clazz, jni_make_string(g_obb_name), 0);
  if (p_setScreenSize)
    p_setScreenSize(fake_env, fake_clazz, screen_width, screen_height);
  if (p_setScreenScaleDesity)
    p_setScreenScaleDesity(fake_env, fake_clazz, 1.0f, 1.0f);

  debugPrintf("calling ANativeActivity_onCreate\n");
  ANativeActivity_onCreate(&g_activity, NULL, 0);

  // pad/touch setup
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  // drive the Android lifecycle on a helper thread
  pthread_t lc_thread;
  pthread_create(&lc_thread, NULL, lifecycle_thread, NULL);

  debugPrintf("entering main input loop\n");
  while (appletMainLoop() && !android_host_finish_requested) {
    try_signal_controller(); // fires deviceAdded once the engine input is ready
    update_gamepad();
    update_touch();
    svcSleepThread(2 * 1000 * 1000); // ~2ms (~500Hz input polling)
  }

  debugPrintf("shutting down\n");
  if (g_callbacks.onWindowFocusChanged) g_callbacks.onWindowFocusChanged(&g_activity, 0);
  if (g_callbacks.onPause) g_callbacks.onPause(&g_activity);
  if (g_callbacks.onNativeWindowDestroyed && g_window) g_callbacks.onNativeWindowDestroyed(&g_activity, g_window);
  if (g_callbacks.onStop) g_callbacks.onStop(&g_activity);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}

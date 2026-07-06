/* stubs.c -- extra bionic/NDK shims specific to After Burner Climax
 *
 * See stubs.h. This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <malloc.h>
#include <unistd.h>
#include <switch.h>

#include "util.h"
#include "android_host.h"
#include "stubs.h"

// ---------------------------------------------------------------------------
// anonymous mmap allocator
//
// The engine's allocator/CRT imports mmap/munmap/mprotect. There is no mmap on
// the Switch (no VM syscall exposed to newlib), and file-backed shared mappings
// are never actually needed by this game, so we back anonymous mappings with
// aligned heap allocations and remember the base->size mapping so munmap frees.
// mprotect/madvise/mlock become no-ops (the memory is already RW).
// ---------------------------------------------------------------------------

#define LINUX_MAP_ANONYMOUS 0x20
#define MMAP_FAILED         ((void *)-1)
#define MMAP_MAX            256

static struct { void *base; size_t size; } g_maps[MMAP_MAX];

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  (void)addr; (void)prot; (void)offset;
  if (length == 0)
    return MMAP_FAILED;
  // only anonymous mappings are supported; a file-backed request means the
  // engine wanted a real mapping we can't provide -- fail so it falls back to
  // read()/fread() (mojito checks mmap and degrades to buffered reads).
  if (fd >= 0 && !(flags & LINUX_MAP_ANONYMOUS))
    return MMAP_FAILED;
  void *p = memalign(0x1000, (length + 0xFFF) & ~(size_t)0xFFF);
  if (!p)
    return MMAP_FAILED;
  memset(p, 0, length);
  for (int i = 0; i < MMAP_MAX; i++) {
    if (!g_maps[i].base) { g_maps[i].base = p; g_maps[i].size = length; break; }
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  (void)length;
  for (int i = 0; i < MMAP_MAX; i++) {
    if (g_maps[i].base == addr) {
      free(addr);
      g_maps[i].base = NULL; g_maps[i].size = 0;
      return 0;
    }
  }
  // not one of ours (or already freed): ignore
  return 0;
}

int mprotect_fake(void *addr, size_t length, int prot) { (void)addr; (void)length; (void)prot; return 0; }
int madvise_fake(void *addr, size_t length, int advice) { (void)addr; (void)length; (void)advice; return 0; }
int mlock_fake(const void *addr, size_t length) { (void)addr; (void)length; return 0; }

// ---------------------------------------------------------------------------
// Android sensors: report "none available". After Burner Climax's mobile build
// supports accelerometer steering, but it also fully supports a physical
// gamepad (the FireTV/console path). With no default sensor the engine uses the
// gamepad/stick input we synthesize.
// ---------------------------------------------------------------------------

void *ASensorManager_getInstance_fake(void) { return (void *)1; }
void *ASensorManager_getDefaultSensor_fake(void *mgr, int type) { (void)mgr; (void)type; return NULL; }
void *ASensorManager_createEventQueue_fake(void *mgr, void *looper, int ident, void *cb, void *data) {
  (void)mgr; (void)looper; (void)ident; (void)cb; (void)data; return NULL;
}
int ASensorEventQueue_enableSensor_fake(void *q, void *sensor) { (void)q; (void)sensor; return -1; }
int ASensorEventQueue_disableSensor_fake(void *q, void *sensor) { (void)q; (void)sensor; return -1; }
int ASensorEventQueue_setEventRate_fake(void *q, void *sensor, int usec) { (void)q; (void)sensor; (void)usec; return -1; }
int ASensorEventQueue_getEvents_fake(void *q, void *events, size_t count) { (void)q; (void)events; (void)count; return 0; }

// ---------------------------------------------------------------------------
// AAssetManager directory enumeration, backed by the real "assets/" folder.
// getNextFileName returns bare file names (no subdirs), matching the NDK.
// ---------------------------------------------------------------------------

typedef struct {
  DIR *dir;
  char base[512];       // "assets/<dirName>"
  char namebuf[512];    // storage handed back by getNextFileName
} FakeAssetDir;

void *AAssetManager_openDir_fake(void *mgr, const char *dirName) {
  (void)mgr;
  FakeAssetDir *ad = calloc(1, sizeof(*ad));
  if (!ad)
    return NULL;
  if (dirName && dirName[0])
    snprintf(ad->base, sizeof(ad->base), "assets/%s", dirName);
  else
    snprintf(ad->base, sizeof(ad->base), "assets");
  ad->dir = opendir(ad->base);
  debugPrintf("AAssetDir: openDir(%s) -> %s\n", dirName ? dirName : "", ad->dir ? "ok" : "MISSING");
  return ad; // NDK returns a valid handle even for a missing dir
}

const char *AAssetDir_getNextFileName_fake(void *assetDir) {
  FakeAssetDir *ad = assetDir;
  if (!ad || !ad->dir)
    return NULL;
  struct dirent *e;
  while ((e = readdir(ad->dir)) != NULL) {
    if (e->d_name[0] == '.') // skip "." ".." and hidden
      continue;
    if (e->d_type == DT_DIR) // NDK lists files only
      continue;
    snprintf(ad->namebuf, sizeof(ad->namebuf), "%s", e->d_name);
    return ad->namebuf;
  }
  return NULL;
}

void AAssetDir_rewind_fake(void *assetDir) {
  FakeAssetDir *ad = assetDir;
  if (ad && ad->dir)
    rewinddir(ad->dir);
}

void AAssetDir_close_fake(void *assetDir) {
  FakeAssetDir *ad = assetDir;
  if (ad) {
    if (ad->dir)
      closedir(ad->dir);
    free(ad);
  }
}

// ---------------------------------------------------------------------------
// fake-fd aware raw write: the glue's command pipe uses fake fds (android_host)
// ---------------------------------------------------------------------------

ssize_t write_fake(int fd, const void *buf, size_t count) {
  if (android_fake_fd_is_fake(fd))
    return android_fake_fd_write(fd, buf, count);
  return write(fd, buf, count);
}

// the glue reads its command pipe with plain read() (not __read_chk), so route
// fake pipe/input fds here or the command channel never drains
ssize_t read_fake(int fd, void *buf, size_t count) {
  if (android_fake_fd_is_fake(fd))
    return android_fake_fd_read(fd, buf, count);
  return read(fd, buf, count);
}

// ---------------------------------------------------------------------------
// POSIX odds and ends -- present so the engine links, harmless on console
// ---------------------------------------------------------------------------

int   poll_fake(void *fds, unsigned long nfds, int timeout) { (void)fds; (void)nfds; (void)timeout; return 0; }
int   ioctl_fake(int fd, unsigned long request, ...) { (void)fd; (void)request; return 0; }
int   sigaction_fake(int sig, const void *act, void *oldact) { (void)sig; (void)act; (void)oldact; return 0; }
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int   ret0_sig(void) { return 0; }

int   fork_fake(void) { errno = ENOSYS; return -1; }
int   kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int   system_fake(const char *cmd) { (void)cmd; return -1; }
int   getrlimit_fake(int resource, void *rlim) { (void)resource; if (rlim) memset(rlim, 0, 16); return 0; }
int   sched_yield_fake(void) { svcSleepThread(0); return 0; }
void *getpwuid_fake(unsigned uid) { (void)uid; return NULL; }
unsigned getuid_fake(void) { return 0; }

// ---------------------------------------------------------------------------
// real nanosleep via the kernel sleep syscall; sigsetjmp/siglongjmp fold onto
// newlib's plain setjmp/longjmp (signal masks are irrelevant on console)
// ---------------------------------------------------------------------------

int nanosleep_fake(const struct timespec *req, struct timespec *rem) {
  if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
  if (!req)
    return 0;
  u64 ns = (u64)req->tv_sec * 1000000000ULL + (u64)req->tv_nsec;
  svcSleepThread(ns);
  return 0;
}

int sigsetjmp_fake(jmp_buf env, int savesigs) { (void)savesigs; return setjmp(env); }
void siglongjmp_fake(jmp_buf env, int val) { longjmp(env, val); }

// ---------------------------------------------------------------------------
// networking: offline. Sockets themselves are stubbed to failure in imports.c;
// name resolution reports "no address" so callers give up cleanly.
// ---------------------------------------------------------------------------

int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  (void)node; (void)service; (void)hints;
  if (res) *res = NULL;
  return -2; // EAI_NONAME
}
void freeaddrinfo_fake(void *res) { (void)res; }
const char *gai_strerror_fake(int errcode) { (void)errcode; return "offline"; }
int getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags) {
  (void)sa; (void)salen; (void)flags;
  if (host && hostlen) host[0] = '\0';
  if (serv && servlen) serv[0] = '\0';
  return -2;
}

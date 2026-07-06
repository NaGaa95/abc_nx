/* libc_shim.c -- bionic-compatible libc wrappers
 *
 * The engine is linked against bionic. Where the bionic and newlib ABIs differ
 * (struct layouts, flag values, missing functions) we convert here; everything
 * that matches is passed straight through from imports.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}

char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}

char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
  (void)dstlen; (void)srclen;
  return strncpy(dst, src, n);
}

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

// COI fortify extras
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}

// bionic signature: (buf, size, count, stream, buf_size) -- buf_size is last
size_t __fread_chk_fake(void *buf, size_t size, size_t n, FILE *f, size_t buflen) {
  (void)buflen;
  return fread(buf, size, n, f);
}

size_t __fwrite_chk_fake(const void *buf, size_t size, size_t n, FILE *f, size_t buflen) {
  (void)buflen;
  return fwrite(buf, size, n, f);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  // FMOD queries ro.build.version.sdk to pick its audio backend (>= 9 -> OpenSL);
  // must report a version or System::init() bails before reaching OpenSL.
  if (name && (strstr(name, "version.sdk") || strstr(name, "sdk_int"))) {
    strcpy(value, "30");
    debugPrintf("__system_property_get(%s) -> 30\n", name);
    return 2;
  }
  debugPrintf("__system_property_get(%s) -> (empty)\n", name ? name : "(null)");
  value[0] = '\0';
  return 0;
}

unsigned long getauxval_fake(unsigned long type) {
  // AT_HWCAP: report the Tegra X1 CPU features (FP|ASIMD|AES|PMULL|SHA1|SHA2|
  // CRC32) or feature-probing libs (FMOD) think NEON is absent
  if (type == 16) return 0xff;   // AT_HWCAP
  if (type == 6)  return 0x1000; // AT_PAGESZ
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID:
      return gettid_fake();
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

int sched_get_priority_max_fake(int policy) {
  (void)policy;
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1;
}

int __register_atfork_fake(void) {
  return 0;
}

int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) {
  // threads never exit cleanly in this port; leak instead of running dtors
  (void)fn; (void)arg; (void)dso;
  return 0;
}

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3;
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:
      debugPrintf("libc: sysconf(%d) -> -1\n", name);
      return -1;
  }
}

long pathconf_fake(const char *path, int name) {
  (void)path; (void)name;
  return -1;
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// The mojito persistent system's "home" directory is empty (Java returns no
// app-data path), so the engine builds bare-absolute save/option paths like
// "/Home/fc_abc_options.bin" and "/F2FSave.bin". fsdev can't resolve a leading
// "/" (no device), so those all fail. Rewrite any such virtual-absolute path to
// be relative to the game folder (cwd), creating the first-level subdir (e.g.
// "Home") so writes succeed and saves persist next to the .nro.
static const char *rewrite_vpath(const char *path, char *buf, size_t bufsz) {
  if (!path || path[0] != '/' || path[1] == '/')
    return path;                          // relative, or "//"/device -- leave it
  const char *rel = path + 1;             // strip the leading '/'
  const char *slash = strchr(rel, '/');
  if (slash) {                            // ensure the first-level subdir exists
    char dir[128];
    size_t dl = (size_t)(slash - rel);
    if (dl && dl < sizeof(dir)) { memcpy(dir, rel, dl); dir[dl] = 0; mkdir(dir, 0777); }
  }
  snprintf(buf, bufsz, "%s", rel);
  return buf;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  char vb[600];
  path = rewrite_vpath(path, vb, sizeof(vb));
  return open(path, convert_open_flags(flags), mode);
}

int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd; // assume AT_FDCWD or absolute paths
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  return open(path, convert_open_flags(flags), mode);
}

int unlinkat_fake(int dirfd, const char *path, int flags) {
  (void)dirfd; (void)flags;
  return unlink(path);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  char vb[600];
  path = rewrite_vpath(path, vb, sizeof(vb));
  struct stat real;
  const int ret = stat(path, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int lstat_fake(const char *path, struct bionic_stat *st) {
  return stat_fake(path, st);
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // NOTE: not thread-safe
  struct dirent *e = readdir((DIR *)dirp);
  if (!e)
    return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return (void *)1;
}

void freelocale_fake(void *loc) {
  (void)loc;
}

void *uselocale_fake(void *loc) {
  (void)loc;
  return (void *)1;
}

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha)
WRAP_ISW_L(iswblank)
WRAP_ISW_L(iswcntrl)
WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower)
WRAP_ISW_L(iswprint)
WRAP_ISW_L(iswpunct)
WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper)
WRAP_ISW_L(iswxdigit)
WRAP_ISW_L(towlower)
WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) {
  (void)loc;
  return strcoll(a, b);
}

size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc;
  return strftime(s, max, fmt, (const struct tm *)tm);
}

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc;
  return strtold(s, end);
}

long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoull(s, end, base);
}

int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) {
  (void)loc;
  return wcscoll(a, b);
}

size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) {
  (void)loc;
  return wcsxfrm(dst, src, n);
}

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  // ascii-ish naive conversion
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved)
    resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

int statvfs_fake(const char *path, void *buf) {
  (void)path;
  memset(buf, 0, 0x70);
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr): the C++ runtime ties
// std::cout/cerr to &__sF[1]/&__sF[2]; absorb those fake FILEs, forward the rest
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#ifdef DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total);
    buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) {
    debugPrintf("stdio: %s", s);
    return 0;
  }
  return fputs(s, f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fclose(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#ifdef DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#ifdef DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int getc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return getc(f);
}

int ungetc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ungetc(c, f);
}

void setbuf_fake(FILE *f, char *buf) {
  if (is_fake_file(f))
    return;
  setbuf(f, buf);
}

// ---------------------------------------------------------------------------
// AAsset emulation: the game's data archive (the .obb) is fopen'd directly; the
// few AAssetManager assets are served from an "assets/" subdir of the folder.
// ---------------------------------------------------------------------------

typedef struct {
  FILE *f;
  long size;
} Asset;

void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env; (void)mgr;
  return (void *)1; // any non-NULL token
}

// large stream buffer for archives: the .obb is read in many small reads/seeks
// The engine reads its ~448 MB OBB with thousands of scattered small
// fseek()+fread()s. On the Switch every such read is an fsdev -> FS-service IPC
// round trip (svcSendSyncRequest), so buffering can't hide it and boot is
// I/O-bound to a crawl (looks like a hang at ~1% CPU). Instead, slurp the whole
// archive into RAM once (fast sequential read) and serve every subsequent open
// from memory via fmemopen -- after that, all archive I/O is instant and never
// touches the filesystem. There is a single OBB, so one global cache suffices.
static void *g_obb_cache = NULL;
static long  g_obb_cache_size = 0;

static int is_archive(const char *path) {
  const char *ext = strrchr(path, '.');
  return ext && (strcasecmp(ext, ".obb") == 0 || strcasecmp(ext, ".pak") == 0);
}

static void load_obb_cache(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }
  long sz = (long)st.st_size;
  void *buf = malloc(sz);
  if (!buf) { close(fd); return; }
  long off = 0;
  while (off < sz) {
    long want = sz - off;
    if (want > (4 << 20)) want = 4 << 20;   // 4 MB chunks: few IPC round trips
    ssize_t got = read(fd, (char *)buf + off, (size_t)want);
    if (got <= 0)
      break;
    off += got;
  }
  close(fd);
  if (off == sz) {
    g_obb_cache = buf;
    g_obb_cache_size = sz;
    debugPrintf("obb cache: loaded %ld MB into RAM\n", sz >> 20);
  } else {
    free(buf);
    debugPrintf("obb cache: read short (%ld/%ld) -- falling back to fsdev\n", off, sz);
  }
}

FILE *fopen_fake(const char *path, const char *mode) {
  char vb[600];
  path = rewrite_vpath(path, vb, sizeof(vb));
  if (is_archive(path) && strchr(mode, 'r')) {
    static Mutex obb_lock; // zero-init = unlocked; serializes the one-time load
    mutexLock(&obb_lock);
    if (!g_obb_cache)
      load_obb_cache(path);
    mutexUnlock(&obb_lock);
    if (g_obb_cache) {
      debugPrintf("fopen(%s) -> RAM cache\n", path);
      return fmemopen(g_obb_cache, g_obb_cache_size, "rb");
    }
  }
  FILE *f = fopen(path, mode);
  debugPrintf("fopen(%s, %s) -> %s\n", path, mode, f ? "ok" : "FAIL");
  return f;
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  char buf[0x400];
  // try assets/<path> first, then the bare path
  FILE *f = NULL;
  snprintf(buf, sizeof(buf), "assets/%s", path);
  f = fopen(buf, "rb");
  if (!f)
    f = fopen(path, "rb");
  debugPrintf("AAsset: open(%s) -> %s\n", path, f ? "ok" : "MISSING");
  if (!f)
    return NULL;
  setvbuf(f, NULL, _IOFBF, 16 * 1024);
  Asset *a = malloc(sizeof(*a));
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  return a;
}

void AAsset_close_fake(void *asset) {
  Asset *a = asset;
  if (a) {
    fclose(a->f);
    free(a);
  }
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset;
  return a ? (int)fread(buf, 1, count, a->f) : -1;
}

long AAsset_seek_fake(void *asset, long off, int whence) {
  Asset *a = asset;
  if (!a || fseek(a->f, off, whence) < 0)
    return -1;
  return ftell(a->f);
}

int64_t AAsset_seek64_fake(void *asset, int64_t off, int whence) {
  return AAsset_seek_fake(asset, (long)off, whence);
}

long AAsset_getLength_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size : 0;
}

int64_t AAsset_getLength64_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size : 0;
}

long AAsset_getRemainingLength_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size - ftell(a->f) : 0;
}

int64_t AAsset_getRemainingLength64_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size - ftell(a->f) : 0;
}

// we don't expose real fds for assets; fail (-1) so the engine uses AAsset_read
int AAsset_openFileDescriptor_fake(void *asset, long *outStart, long *outLength) {
  (void)asset; (void)outStart; (void)outLength;
  return -1;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection (the game's
// opaque bionic objects hold a pointer to the real one, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  RwLock lock;
} FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_rdlock_fake(void **rw) {
  rwlockReadLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  rwlockWriteLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  // libnx needs to know which way it was locked
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock))
    rwlockWriteUnlock(&l->lock);
  else
    rwlockReadUnlock(&l->lock);
  return 0;
}

// bionic rwlock is a pointer-indirect opaque like the mutex fakes: init just
// clears the storage so the first lock lazily creates the real RwLock.
int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  if (rw) *rw = NULL;
  return 0;
}

int pthread_rwlock_destroy_fake(void **rw) {
  if (rw && *rw) {
    free(*rw);
    *rw = NULL;
  }
  return 0;
}

typedef struct {
  Semaphore sem;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) {
    free(*s);
    *s = NULL;
  }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s)
    semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s)
    semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem))
    return 0;
  errno = EAGAIN;
  return -1;
}

int sem_getvalue_fake(void **s, int *val) {
  if (s && *s)
    *val = (int)((FakeSem *)*s)->sem.count;
  else
    *val = 0;
  return 0;
}

int pthread_attr_getstacksize_fake(const void *attr, size_t *size) {
  (void)attr;
  *size = 512 * 1024;
  return 0;
}

// ---------------------------------------------------------------------------
// pthread TLS keys (pthread_key_create/get/set/delete)
//
// newlib's implementation follows the thread pointer (TPIDR_EL0), but we
// deliberately overwrite TPIDR_EL0 on every engine thread with a small fake
// bionic TLS (so the engine's stack-guard cookie at TPIDR+0x28 stays stable).
// That makes the stock pthread_getspecific read a bogus reent and crash the
// moment the engine's PARALLEL_OBJECT thread-local mechanism uses it. So we
// keep our own (thread, key) table, keyed on each thread's unique fake-TLS
// pointer -- no dependency on newlib's TLS.
// ---------------------------------------------------------------------------
#define PTK_KEYS    256
#define PTK_THREADS 64
static void    *g_ptk_val[PTK_KEYS][PTK_THREADS];
static uint64_t g_ptk_tid[PTK_THREADS];
static uint8_t  g_ptk_used[PTK_KEYS];
static Mutex    g_ptk_lock;

static int ptk_slot(void) {
  const uint64_t self = (uint64_t)armGetTlsRw(); // unique per thread
  for (int i = 0; i < PTK_THREADS; i++)
    if (g_ptk_tid[i] == self) return i;          // fast path, no lock
  mutexLock(&g_ptk_lock);
  for (int i = 0; i < PTK_THREADS; i++)
    if (g_ptk_tid[i] == self) { mutexUnlock(&g_ptk_lock); return i; }
  for (int i = 0; i < PTK_THREADS; i++)
    if (g_ptk_tid[i] == 0) { g_ptk_tid[i] = self; mutexUnlock(&g_ptk_lock); return i; }
  mutexUnlock(&g_ptk_lock);
  return 0; // out of slots (>64 threads) -- shouldn't happen
}

int pthread_key_create_fake(unsigned *key, void *dtor) {
  (void)dtor; // threads never exit cleanly here; destructors are leaked
  int k = -1;
  mutexLock(&g_ptk_lock);
  for (int i = 0; i < PTK_KEYS; i++)
    if (!g_ptk_used[i]) { g_ptk_used[i] = 1; k = i; break; }
  mutexUnlock(&g_ptk_lock);
  if (k < 0) return 11; // EAGAIN
  for (int t = 0; t < PTK_THREADS; t++) g_ptk_val[k][t] = NULL;
  if (key) *key = (unsigned)(k + 1); // 1-based: keep 0 as "no key"
  return 0;
}

int pthread_key_delete_fake(unsigned key) {
  if (key >= 1 && key <= PTK_KEYS) g_ptk_used[key - 1] = 0;
  return 0;
}

int pthread_setspecific_fake(unsigned key, const void *val) {
  if (key < 1 || key > PTK_KEYS) return 22; // EINVAL
  g_ptk_val[key - 1][ptk_slot()] = (void *)val;
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key < 1 || key > PTK_KEYS) return NULL;
  return g_ptk_val[key - 1][ptk_slot()];
}

int pthread_attr_getschedparam_fake(const void *attr, void *param) {
  (void)attr;
  memset(param, 0, 8);
  return 0;
}

/* imports.c -- libViewer_GP.so import resolution (Castle of Illusion)
 *
 * The dynlib_functions table below resolves every UND import of the engine:
 * NDK NativeActivity APIs (android_host.c), libc/bionic shims (libc_shim.c +
 * newlib), EGL/GLES2 (switch-mesa), the GNU C++ runtime, and FMOD Ex.
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
#include <unistd.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <locale.h>
#include <setjmp.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <malloc.h>
#include <semaphore.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "android_host.h"
#include "opensl.h"
#include "fmv.h"
#include "fps_overlay.h"
#include "stubs.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;

static char *__ctype_ptr_ = (char *)&_ctype_;
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

// bionic stdio globals the engine references directly. Point them at the fake
// __sF FILEs so writes are swallowed (is_fake_file), never a real FILE* deref.
FILE *stderr_fake = (FILE *)fake_sF[2];
static FILE *stdout_fake = (FILE *)fake_sF[1];

// ---------------------------------------------------------------------------
// logging / assert
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  if (fmt && strstr(fmt, "New input event")) return 0; // glue spam
  // format unconditionally (boost_scan_log must run in release too); local buf = thread-safe
  char string[0x1000];
  va_list list;
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  boost_scan_log(string);
#ifdef DEBUG_LOG
  debugPrintf("%s: %s\n", tag, string);
#else
  (void)tag;
#endif
  return 0;
}

static void __assert2(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func ? func : "?", expr);
}

int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  boost_scan_log(text);
#ifdef DEBUG_LOG
  debugPrintf("%s: %s\n", tag ? tag : "?", text ? text : "");
#else
  (void)tag; (void)text;
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// dlopen/dlsym: route FMOD's dlopen("libOpenSLES.so") to the audout bridge
// (opensl.c). Other libs get a generic non-NULL handle with dlsym -> NULL.
// ---------------------------------------------------------------------------

#define DL_HANDLE_OPENSLES ((void *)0x5151)
#define DL_HANDLE_GENERIC  ((void *)0x5150)

static void *dlopen_fake(const char *name, int flag) {
  (void)flag;
  if (name && strstr(name, "OpenSLES")) {
    debugPrintf("dlopen(%s) -> OpenSL bridge\n", name);
    return DL_HANDLE_OPENSLES;
  }
  debugPrintf("dlopen(%s) -> generic\n", name ? name : "(null)");
  return DL_HANDLE_GENERIC;
}

static void *dlsym_fake(void *handle, const char *name) {
  if (handle == DL_HANDLE_OPENSLES)
    return opensl_dlsym(name);
  return NULL;
}

// Shadow FMOD::System::init (called by libfmodstudio) to force OpenSL ES output.
// Autodetect otherwise picks AudioTrack (Java-only, unavailable here) and fails.
// This build's FMOD_OUTPUTTYPE enum is non-standard: 16 = OpenSL. Reals in main.c.
int (*g_real_fmod_System_init)(void *sys, int maxch, unsigned flags, void *extra);
int (*g_real_fmod_System_setOutput)(void *sys, int out);

#define FMOD_OUTPUTTYPE_OPENSL 16

static int fmod_System_init_hook(void *sys, int maxch, unsigned flags, void *extra) {
  if (g_real_fmod_System_setOutput) g_real_fmod_System_setOutput(sys, FMOD_OUTPUTTYPE_OPENSL);
  return g_real_fmod_System_init ? g_real_fmod_System_init(sys, maxch, flags, extra) : -1;
}

static int dlclose_fake(void *handle) { (void)handle; return 0; }
static const char *dlerror_fake(void) { return NULL; }

// fortified open used by libfmodex for non-O_CREAT opens
static int __open_2(const char *path, int flags) { return open(path, flags); }

// no-op on the SD fs
static int utime_fake(const char *path, const void *times) { (void)path; (void)times; return 0; }
static int chown_fake(const char *path, unsigned uid, unsigned gid) {
  (void)path; (void)uid; (void)gid; return 0;
}

// bionic pthread_once_t is a plain int; newlib's is a wider struct, so don't
// route to newlib. A guarded one-shot is all FMOD's single-threaded init needs.
static int pthread_once_fake(int *once, void (*init)(void)) {
  if (once && __atomic_exchange_n(once, 1, __ATOMIC_SEQ_CST) == 0 && init)
    init();
  return 0;
}

// ---------------------------------------------------------------------------
// fortified read/write/close/fcntl: the glue's command pipe uses fake fds
// (android_host.c); everything else is a real newlib fd.
// ---------------------------------------------------------------------------

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  if (android_fake_fd_is_fake(fd))
    return android_fake_fd_read(fd, buf, count);
  return read(fd, buf, count);
}

ssize_t __write_chk(int fd, const void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  if (android_fake_fd_is_fake(fd))
    return android_fake_fd_write(fd, buf, count);
  return write(fd, buf, count);
}

static int close_fake(int fd) {
  if (android_fake_fd_is_fake(fd))
    return android_fake_fd_close(fd);
  return close(fd);
}

static int fcntl_fake(int fd, int cmd, ...) {
  if (android_fake_fd_is_fake(fd))
    return 0; // O_NONBLOCK etc. on the fake pipe: accept silently
  va_list va;
  va_start(va, cmd);
  void *arg = va_arg(va, void *);
  va_end(va);
  return fcntl(fd, cmd, arg);
}

// ---------------------------------------------------------------------------
// pthread wrappers: bionic struct layouts differ from newlib, so the game's
// opaque pthread objects (>= 8 bytes on arm64) hold a pointer to the real one.
// ---------------------------------------------------------------------------

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (attr && *attr == 1); // bionic PTHREAD_MUTEX_RECURSIVE
  // honor recursive: the engine's CriticalSection self-deadlocks without it
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  if (recursive)
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  int ret = pthread_mutex_init(m, &a);
  pthread_mutexattr_destroy(&a);
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}
static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int a = 1; ret = pthread_mutex_init_fake(uid, &a); }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int a = 1; ret = pthread_mutex_init_fake(uid, &a); }
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}
static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  *c = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}
static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; }
  return 0;
}
static int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_signal(*cnd);
}
static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}
static int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *abstime) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_timedwait(*cnd, *mtx, abstime);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int a = 1; ret = pthread_mutex_init_fake(uid, &a); }
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}
// Game code reads its stack-guard cookie from TPIDR_EL0+40 (bionic TLS), which
// the Switch leaves unset, so install a zeroed bionic-TLS buffer on each
// spawned thread before it enters game/glue code. Leaked on thread exit.
typedef struct {
  void *(*entry)(void *);
  void *arg;
  uint8_t tls[0x100];
} ThreadStart;

static void *pthread_trampoline(void *p) {
  ThreadStart *s = p;
  void *(*entry)(void *) = s->entry;
  void *arg = s->arg;
  memset(s->tls, 0, sizeof(s->tls));
  armSetTlsRw(s->tls);
  return entry(arg);
}

// bionic mutexattr is a small int we stash the type in; pthread_mutex_init_fake
// reads *attr == 1 (PTHREAD_MUTEX_RECURSIVE) to build a recursive mutex.
static int pthread_mutexattr_init_fake(int *attr) {
  if (attr) *attr = 0; // PTHREAD_MUTEX_NORMAL
  return 0;
}
static int pthread_mutexattr_settype_fake(int *attr, int type) {
  if (attr) *attr = type;
  return 0;
}

// The engine creates its worker threads with the *default* stack size (it never
// calls pthread_attr_setstacksize), relying on Android's ~1 MB default. newlib's
// default is only ~128 KB, which the engine's data loader / MEMORY allocator
// pool-growth recursion overflows. Give every engine thread a generous stack.
#define ENGINE_THREAD_STACK (8 * 1024 * 1024)

static int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  ThreadStart *s = calloc(1, sizeof(*s));
  if (!s)
    return -1;
  s->entry = entry;
  s->arg = arg;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, ENGINE_THREAD_STACK);
  int rc = pthread_create(thread, &attr, pthread_trampoline, s);
  pthread_attr_destroy(&attr);
  debugPrintf("pthread_create(entry=%p) -> rc=%d tid=%lu\n", entry, rc, thread ? (unsigned long)*thread : 0);
  if (rc != 0)
    free(s);
  return rc;
}

// ---------------------------------------------------------------------------
// sockets: stub the BSD layer to failure (COI only uses it for optional
// telemetry, which degrades gracefully when offline)
// ---------------------------------------------------------------------------

static int socket_fail(void) { errno = EAFNOSUPPORT; return -1; }
static void *null_ptr(void) { return NULL; }
static int select_fake(int n, void *r, void *w, void *e, void *t) {
  (void)n; (void)r; (void)w; (void)e; (void)t;
  return 0; // nothing ready
}
static void fd_noop(void) {}

// ---------------------------------------------------------------------------
// misc libc bits
// ---------------------------------------------------------------------------

static int getpid_fake(void) { return 1; }
static void sincosf_local(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

// ---------------------------------------------------------------------------
// GL hooks (optional quality tweaks) + the OES program-binary fallback
// ---------------------------------------------------------------------------

static void glTexParameteriHook(GLenum target, GLenum param, GLint val) {
  if (val == GL_LINEAR_MIPMAP_NEAREST) // force trilinear
    val = GL_LINEAR_MIPMAP_LINEAR;
  glTexParameteri(target, param, val);
}

// the engine's precompiled GLES program binaries (Android driver format) can't
// load on mesa; no-op so the program stays unlinked and the engine recompiles.
static void glProgramBinaryOES_stub(GLuint program, GLenum binaryFormat,
    const void *binary, GLint length) {
  (void)program; (void)binaryFormat; (void)binary; (void)length;
}

static void glGetShaderInfoLogHook(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
  glGetShaderInfoLog(shader, maxLength, length, infoLog);
  debugPrintf("shader info log:\n%s\n", infoLog);
}

// ---------------------------------------------------------------------------
// EGL/GL diagnostics: trace the graphics bring-up. The thread id is logged
// because a GL context is current per-thread.
// ---------------------------------------------------------------------------

static unsigned cur_tid(void) {
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);
  return (unsigned)tid;
}

// The game's config-selection yields NULL on mesa and then passes that NULL
// config on -> EGL_BAD_CONFIG. Pick a good ES2 config and substitute it for NULL.
static EGLConfig g_egl_config = NULL;
static EGLDisplay g_egl_dpy = NULL;

static void pick_egl_config(EGLDisplay dpy) {
  const EGLint attribs[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE,   8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE,  8,
    EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24,
    EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg = NULL;
  EGLint n = 0;
  if (eglChooseConfig(dpy, attribs, &cfg, 1, &n) && n > 0) {
    g_egl_config = cfg;
  } else {
    // relax depth/stencil if the exact combo isn't available
    const EGLint relaxed[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_DEPTH_SIZE, 16, EGL_NONE
    };
    if (eglChooseConfig(dpy, relaxed, &cfg, 1, &n) && n > 0)
      g_egl_config = cfg;
  }
  g_egl_dpy = dpy;
  debugPrintf("EGL: picked fallback config=%p (n=%d)\n", (void *)g_egl_config, n);
}

static EGLDisplay eglGetDisplayHook(EGLNativeDisplayType dpy) {
  EGLDisplay d = eglGetDisplay(dpy);
  debugPrintf("EGL[t%u]: eglGetDisplay(%p) -> %p\n", cur_tid(), (void *)dpy, (void *)d);
  return d;
}
static EGLBoolean eglInitializeHook(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  EGLBoolean r = eglInitialize(dpy, major, minor);
  // the game never calls eglBindAPI; bind ES or mesa defaults to desktop GL and
  // rejects the ES config (re-bound per-thread at context creation)
  EGLBoolean bound = eglBindAPI(EGL_OPENGL_ES_API);
  debugPrintf("EGL: eglInitialize -> %d (v%d.%d) bindES=%d err=0x%x\n", r,
              major ? *major : -1, minor ? *minor : -1, bound, eglGetError());
  if (r)
    pick_egl_config(dpy);
  return r;
}
static EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib,
    EGLConfig *configs, EGLint sz, EGLint *num) {
  EGLBoolean r = eglChooseConfig(dpy, attrib, configs, sz, num);
  debugPrintf("EGL: eglChooseConfig -> %d num=%d err=0x%x\n", r, num ? *num : -1, eglGetError());
  return r;
}
static EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attrib) {
  eglBindAPI(EGL_OPENGL_ES_API); // per-thread; ensure ES is current here too
  if (!cfg && g_egl_config) { cfg = g_egl_config; debugPrintf("EGL: substituting config for NULL\n"); }
  EGLContext c = eglCreateContext(dpy, cfg, share, attrib);
  debugPrintf("EGL: eglCreateContext(cfg=%p) -> %p err=0x%x\n", (void *)cfg, (void *)c, eglGetError());
  return c;
}
static EGLSurface eglCreateWindowSurfaceHook(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win, const EGLint *attrib) {
  if (!cfg && g_egl_config) { cfg = g_egl_config; debugPrintf("EGL: substituting config for NULL (surface)\n"); }
  // The engine calls ANativeWindow_setBuffersGeometry(0,0) meaning "use the
  // native default size"; make sure the NWindow is actually sized to our render
  // resolution before the surface is created, or mesa builds a 0x0 surface and
  // the engine renders to nothing (black).
  if (win)
    nwindowSetDimensions((NWindow *)win, screen_width, screen_height);
  debugPrintf("EGL[t%u]: eglCreateWindowSurface(cfg=%p win=%p) ...\n", cur_tid(), (void *)cfg, (void *)win);
  EGLSurface s = eglCreateWindowSurface(dpy, cfg, win, attrib);
  EGLint sw = 0, sh = 0;
  if (s) { eglQuerySurface(dpy, s, EGL_WIDTH, &sw); eglQuerySurface(dpy, s, EGL_HEIGHT, &sh); }
  debugPrintf("EGL[t%u]: eglCreateWindowSurface -> %p (%dx%d) err=0x%x\n", cur_tid(), (void *)s, sw, sh, eglGetError());
  return s;
}
// The engine reads the framebuffer size back via eglQuerySurface; if mesa hasn't
// settled the size yet it can report 0, so substitute our render resolution.
static EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surf, EGLint attr, EGLint *value) {
  EGLBoolean r = eglQuerySurface(dpy, surf, attr, value);
  if (value) {
    if (attr == EGL_WIDTH  && *value <= 0) { *value = screen_width;  r = EGL_TRUE; }
    if (attr == EGL_HEIGHT && *value <= 0) { *value = screen_height; r = EGL_TRUE; }
  }
  return r;
}
static EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  EGLBoolean r = eglMakeCurrent(dpy, draw, read, ctx);
#ifdef DEBUG_LOG
  // The engine's two GL threads swap the context ~thousands of times; logging
  // (an SD write) per call would dwarf the actual work. Log only the first.
  static int logged = 0;
  if (r && ctx && !logged) {
    logged = 1;
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *rend = (const char *)glGetString(GL_RENDERER);
    debugPrintf("EGL: first makeCurrent ok; GL version=%s renderer=%s\n", ver ? ver : "?", rend ? rend : "?");
  }
#endif
  return r;
}
static EGLBoolean eglSwapBuffersHook(EGLDisplay dpy, EGLSurface surf) {
  fmv_render_pump(); // play the SEGA intro logo here if the engine requested it
  if (config.show_fps)
    fps_overlay_render();
  return eglSwapBuffers(dpy, surf);
}
static void glClearHook(GLbitfield mask) {
  glClear(mask);
  static int once = 0;
  if (!once) { once = 1; debugPrintf("GL: first glClear(0x%x) err=0x%x\n", mask, glGetError()); }
}

// engine does strstr/strlen on glGetString results without null-checking; mesa
// returns NULL with no context, so never hand it NULL
static const GLubyte *glGetStringHook(GLenum name) {
  const GLubyte *s = glGetString(name);
  return s ? s : (const GLubyte *)"";
}

// ---------------------------------------------------------------------------
// C++ runtime (GNU libstdc++/libsupc++): declare the imported symbols so the
// linker pulls them in; the table stores their addresses.
// ---------------------------------------------------------------------------

extern void __cxa_throw(void *, void *, void *);
extern void *__cxa_allocate_exception(size_t);
// C++ ABI bits libfmod/libfmodstudio import; supplied by the linked libstdc++
extern void _ZdlPv(void *);              // operator delete(void*)
extern int __cxa_guard_acquire(void *);  // one-time static init guard
extern void __cxa_guard_release(void *);
extern void __cxa_free_exception(void *);
extern void *__cxa_begin_catch(void *);
extern void __cxa_end_catch(void);
extern void __cxa_rethrow(void);
extern void __cxa_pure_virtual(void);
extern int __gxx_personality_v0(int, int, uint64_t, void *, void *);
extern void _ZSt9terminatev(void);          // std::terminate()
extern void _ZNSt9exceptionD2Ev(void *);     // std::exception::~exception()
extern char _ZTISt9exception[];              // typeinfo for std::exception
extern char _ZTIi[], _ZTIj[], _ZTIf[], _ZTIb[];
extern char _ZTVN10__cxxabiv116__enum_type_infoE[];
extern char _ZTVN10__cxxabiv117__class_type_infoE[];
extern char _ZTVN10__cxxabiv120__si_class_type_infoE[];
extern char _ZTVN10__cxxabiv121__vmi_class_type_infoE[];

// newlib exposes these as macros; drop the macros so we can take the address of
// the underlying extern functions in the import table below.
#undef isalnum
#undef isalpha
#undef isascii
#undef iscntrl
#undef islower
#undef isprint
#undef ispunct
#undef isspace
#undef isupper
#undef isxdigit
#undef tolower
#undef toupper
#undef iswalpha
#undef iswblank
#undef iswcntrl
#undef iswdigit
#undef iswlower
#undef iswprint
#undef iswpunct
#undef iswspace
#undef iswupper
#undef iswxdigit
#undef towlower
#undef towupper
#undef putchar
#undef btowc
#undef wctob

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- bionic stdio globals & logging ---
  { "__sF", (uintptr_t)&fake_sF },
  { "stdout", (uintptr_t)&stdout_fake },
  { "stderr", (uintptr_t)&stderr_fake },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__assert2", (uintptr_t)&__assert2 },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },

  // --- C runtime / errno / stack guard ---
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "_ctype_", (uintptr_t)&__ctype_ptr_ },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "abort", (uintptr_t)&abort },
  { "getpid", (uintptr_t)&getpid_fake },

  // --- fortify wrappers ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk },
  { "__write_chk", (uintptr_t)&__write_chk },
  { "__fread_chk", (uintptr_t)&__fread_chk_fake },
  { "__fwrite_chk", (uintptr_t)&__fwrite_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&fd_noop },
  { "__FD_ISSET_chk", (uintptr_t)&ret0 },
  { "__recvfrom_chk", (uintptr_t)&socket_fail },
  { "__sendto_chk", (uintptr_t)&socket_fail },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc },
  { "free", (uintptr_t)&free },

  // --- math ---
  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "atanf", (uintptr_t)&atanf },
  { "atan2f", (uintptr_t)&atan2f },
  { "cosf", (uintptr_t)&cosf },
  { "sinf", (uintptr_t)&sinf },
  { "tanf", (uintptr_t)&tanf },
  { "powf", (uintptr_t)&powf },
  { "fmodf", (uintptr_t)&fmodf },
  { "log", (uintptr_t)&log },
  { "sin", (uintptr_t)&sin },
  { "sincosf", (uintptr_t)&sincosf_local },

  // --- string / wchar / ctype ---
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strerror", (uintptr_t)&strerror },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtok", (uintptr_t)&strtok },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "atof", (uintptr_t)&atof },
  { "atoi", (uintptr_t)&atoi },
  { "rand", (uintptr_t)&rand },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },

  // --- formatted I/O ---
  { "printf", (uintptr_t)&debugPrintf },
  { "snprintf", (uintptr_t)&snprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },

  // --- stdio file I/O ---
  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell },

  // --- low-level fds (fake-pipe aware) ---
  { "close", (uintptr_t)&close_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "pipe", (uintptr_t)&android_pipe_fake },

  // --- time / sleep ---
  { "clock_gettime", (uintptr_t)&clock_gettime },
  { "time", (uintptr_t)&time },
  { "usleep", (uintptr_t)&usleep },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_condattr_init", (uintptr_t)&ret0 },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // --- sockets (stubbed to offline) ---
  { "socket", (uintptr_t)&socket_fail },
  { "accept", (uintptr_t)&socket_fail },
  { "bind", (uintptr_t)&socket_fail },
  { "connect", (uintptr_t)&socket_fail },
  { "listen", (uintptr_t)&socket_fail },
  { "recvfrom", (uintptr_t)&socket_fail },
  { "sendto", (uintptr_t)&socket_fail },
  { "setsockopt", (uintptr_t)&socket_fail },
  { "shutdown", (uintptr_t)&socket_fail },
  { "select", (uintptr_t)&select_fake },
  { "gethostbyname", (uintptr_t)&null_ptr },
  { "inet_addr", (uintptr_t)&retm1 },
  { "inet_ntoa", (uintptr_t)&null_ptr },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },

  // --- NDK NativeActivity APIs (android_host.c) ---
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },

  { "AConfiguration_new", (uintptr_t)&AConfiguration_new },
  { "AConfiguration_delete", (uintptr_t)&AConfiguration_delete },
  { "AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager },
  { "AConfiguration_getMcc", (uintptr_t)&AConfiguration_getMcc },
  { "AConfiguration_getMnc", (uintptr_t)&AConfiguration_getMnc },
  { "AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage },
  { "AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry },
  { "AConfiguration_getOrientation", (uintptr_t)&AConfiguration_getOrientation },
  { "AConfiguration_getTouchscreen", (uintptr_t)&AConfiguration_getTouchscreen },
  { "AConfiguration_getDensity", (uintptr_t)&AConfiguration_getDensity },
  { "AConfiguration_getKeyboard", (uintptr_t)&AConfiguration_getKeyboard },
  { "AConfiguration_getNavigation", (uintptr_t)&AConfiguration_getNavigation },
  { "AConfiguration_getKeysHidden", (uintptr_t)&AConfiguration_getKeysHidden },
  { "AConfiguration_getNavHidden", (uintptr_t)&AConfiguration_getNavHidden },
  { "AConfiguration_getSdkVersion", (uintptr_t)&AConfiguration_getSdkVersion },
  { "AConfiguration_getScreenSize", (uintptr_t)&AConfiguration_getScreenSize },
  { "AConfiguration_getScreenLong", (uintptr_t)&AConfiguration_getScreenLong },
  { "AConfiguration_getUiModeType", (uintptr_t)&AConfiguration_getUiModeType },
  { "AConfiguration_getUiModeNight", (uintptr_t)&AConfiguration_getUiModeNight },

  { "AInputEvent_getType", (uintptr_t)&AInputEvent_getType },
  { "AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper },
  { "AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper },
  { "AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent },
  { "AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent },
  { "AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent },
  { "AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction },
  { "AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode },
  { "AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction },
  { "AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue },
  { "AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount },
  { "AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId },
  { "AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX },
  { "AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY },

  { "ALooper_prepare", (uintptr_t)&ALooper_prepare },
  { "ALooper_addFd", (uintptr_t)&ALooper_addFd },
  { "ALooper_pollAll", (uintptr_t)&ALooper_pollAll },
  { "ALooper_wake", (uintptr_t)&ALooper_wake },

  { "ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish },
  { "ANativeActivity_setWindowFlags", (uintptr_t)&ANativeActivity_setWindowFlags },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },

  // --- EGL (switch-mesa); hooked for boot diagnostics ---
  { "eglGetDisplay", (uintptr_t)&eglGetDisplayHook },
  { "eglInitialize", (uintptr_t)&eglInitializeHook },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfigHook },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateContext", (uintptr_t)&eglCreateContextHook },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurfaceHook },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrentHook },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffersHook },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext },

  // --- GLES2 (switch-mesa) ---
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClear", (uintptr_t)&glClearHook },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDetachShader", (uintptr_t)&glDetachShader },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetStringHook },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
  { "glProgramBinaryOES", (uintptr_t)&glProgramBinaryOES_stub },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
  { "glStencilMaskSeparate", (uintptr_t)&glStencilMaskSeparate },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameterf", (uintptr_t)&glTexParameterf },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniform4i", (uintptr_t)&glUniform4i },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },

  // --- C++ runtime (GNU libstdc++/libsupc++) ---
  { "__cxa_throw", (uintptr_t)&__cxa_throw },
  { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
  { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
  { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
  { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
  { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
  { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
  { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
  { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
  { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
  { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
  { "_ZTIi", (uintptr_t)&_ZTIi },
  { "_ZTIj", (uintptr_t)&_ZTIj },
  { "_ZTIf", (uintptr_t)&_ZTIf },
  { "_ZTIb", (uintptr_t)&_ZTIb },
  { "_ZTVN10__cxxabiv116__enum_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv116__enum_type_infoE },
  { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
  { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
  { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },

  // --- libfmodex.so support ---------------------------------------------
  // extra libc/math/pthread/dl symbols libfmodex itself imports, plus the
  // dlopen/dlsym hooks that route its OpenSL ES output to opensl.c.
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  // shadow FMOD::System::init to force the OpenSL output (real fn from fmod_mod)
  { "_ZN4FMOD6System4initEijPv", (uintptr_t)&fmod_System_init_hook },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__open_2", (uintptr_t)&__open_2 },
  { "utime", (uintptr_t)&utime_fake },
  { "sincos", (uintptr_t)&sincos },
  { "sincosf", (uintptr_t)&sincosf },
  // math (newlib libm)
  { "atan2", (uintptr_t)&atan2 },
  { "cos", (uintptr_t)&cos },
  { "exp", (uintptr_t)&exp },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "frexp", (uintptr_t)&frexp },
  { "ldexp", (uintptr_t)&ldexp },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "pow", (uintptr_t)&pow },
  // libc (newlib)
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "qsort", (uintptr_t)&qsort },
  { "puts", (uintptr_t)&puts },
  { "read", (uintptr_t)&read_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "rename", (uintptr_t)&rename },
  { "unlink", (uintptr_t)&unlink },
  { "chmod", (uintptr_t)&chmod },
  { "chown", (uintptr_t)&chown_fake },
  { "strcat", (uintptr_t)&strcat },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftello", (uintptr_t)&ftello },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  // pthread / semaphore (newlib)
  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  { "pthread_attr_setstacksize", (uintptr_t)&ret0 },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // =====================================================================
  // After Burner Climax (libacb.so) additions -- wider import surface than
  // Castle of Illusion: split FMOD (studio) cross-resolves; the rest is
  // newlib/mesa pass-through plus console-safe stubs (stubs.c/libc_shim.c).
  // =====================================================================

  // --- NDK: sensors (report none -> engine uses the gamepad path) ---
  { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance_fake },
  { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor_fake },
  { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue_fake },
  { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor_fake },
  { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor_fake },
  { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate_fake },
  { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents_fake },

  // --- NDK: activity/looper/config/asset extras ---
  { "AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource },
  { "ALooper_forThread", (uintptr_t)&ALooper_forThread },
  { "AConfiguration_setOrientation", (uintptr_t)&AConfiguration_setOrientation },
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close_fake },

  // --- EGL extras (switch-mesa) ---
  { "eglQuerySurface", (uintptr_t)&eglQuerySurfaceHook },
  { "eglTerminate", (uintptr_t)&eglTerminate },

  // --- GLES2 extras (switch-mesa) ---
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFlush", (uintptr_t)&glFlush },
  { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
  { "glGetAttachedShaders", (uintptr_t)&glGetAttachedShaders },
  { "glGetFramebufferAttachmentParameteriv", (uintptr_t)&glGetFramebufferAttachmentParameteriv },
  { "glGetRenderbufferParameteriv", (uintptr_t)&glGetRenderbufferParameteriv },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glScissor", (uintptr_t)&glScissor },
  { "glUniform1iv", (uintptr_t)&glUniform1iv },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },

  // --- anonymous mmap allocator + memory (stubs.c) ---
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "madvise", (uintptr_t)&madvise_fake },
  { "mlock", (uintptr_t)&mlock_fake },

  // --- math (newlib libm) ---
  { "acoshf", (uintptr_t)&acoshf },
  { "asinhf", (uintptr_t)&asinhf },
  { "atanhf", (uintptr_t)&atanhf },
  { "coshf", (uintptr_t)&coshf },
  { "sinhf", (uintptr_t)&sinhf },
  { "tanhf", (uintptr_t)&tanhf },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "frexpf", (uintptr_t)&frexpf },
  { "modff", (uintptr_t)&modff },

  // --- stdlib / stdio ---
  { "atol", (uintptr_t)&atol },
  { "atoll", (uintptr_t)&atoll },
  { "srand", (uintptr_t)&srand },
  { "exit", (uintptr_t)&exit },
  { "_exit", (uintptr_t)&exit },
  { "getenv", (uintptr_t)&getenv },
  { "getcwd", (uintptr_t)&getcwd },
  { "chdir", (uintptr_t)&chdir },
  { "sprintf", (uintptr_t)&sprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "vswprintf", (uintptr_t)&vswprintf },
  { "putchar", (uintptr_t)&putchar },
  { "perror", (uintptr_t)&ret0 },
  { "write", (uintptr_t)&write_fake },
  { "fgets", (uintptr_t)&fgets },
  { "fgetc", (uintptr_t)&getc_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "freopen", (uintptr_t)&freopen },
  { "funopen", (uintptr_t)&funopen },
  { "remove", (uintptr_t)&remove },
  { "rewind", (uintptr_t)&rewind },

  // --- filesystem / dirents ---
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake },
  { "lseek", (uintptr_t)&lseek },
  { "mkdir", (uintptr_t)&mkdir },
  { "rmdir", (uintptr_t)&rmdir },

  // --- ctype ---
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "isascii", (uintptr_t)&isascii },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },

  // --- wide char / ctype ---
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },

  // --- locale ---
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },

  // --- string ---
  { "strcoll", (uintptr_t)&strcoll },
  { "strcspn", (uintptr_t)&strcspn },
  { "strncat", (uintptr_t)&strncat },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strspn", (uintptr_t)&strspn },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strerror_r", (uintptr_t)&strerror_r_fake },

  // --- time ---
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime },
  { "asctime", (uintptr_t)&asctime },
  { "strftime", (uintptr_t)&strftime },
  { "nanosleep", (uintptr_t)&nanosleep_fake },

  // --- setjmp ---
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },
  { "sigsetjmp", (uintptr_t)&sigsetjmp_fake },
  { "siglongjmp", (uintptr_t)&siglongjmp_fake },

  // --- pthread extras ---
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },

  // --- process / signals / tty (console-safe stubs) ---
  { "fork", (uintptr_t)&fork_fake },
  { "execle", (uintptr_t)&retm1 },
  { "kill", (uintptr_t)&kill_fake },
  { "system", (uintptr_t)&system_fake },
  { "getuid", (uintptr_t)&getuid_fake },
  { "setuid", (uintptr_t)&ret0 },
  { "setgid", (uintptr_t)&ret0 },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "initgroups", (uintptr_t)&ret0 },
  { "getrlimit", (uintptr_t)&getrlimit_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigemptyset", (uintptr_t)&ret0_sig },
  { "sigfillset", (uintptr_t)&ret0_sig },
  { "sigdelset", (uintptr_t)&ret0_sig },
  { "sigprocmask", (uintptr_t)&ret0_sig },
  { "tcgetattr", (uintptr_t)&ret0 },
  { "tcsetattr", (uintptr_t)&ret0 },
  { "ioctl", (uintptr_t)&ioctl_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "dup", (uintptr_t)&retm1 },
  { "dup2", (uintptr_t)&retm1 },
  { "__FD_CLR_chk", (uintptr_t)&fd_noop },

  // --- networking: offline (name resolution fails; sockets fail) ---
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake },
  { "inet_pton", (uintptr_t)&ret0 },
  { "inet_ntop", (uintptr_t)&null_ptr },
  { "recv", (uintptr_t)&socket_fail },
  { "send", (uintptr_t)&socket_fail },
  { "getpeername", (uintptr_t)&socket_fail },
  { "getsockname", (uintptr_t)&socket_fail },
  { "getsockopt", (uintptr_t)&socket_fail },

  // --- FMOD (libfmod/libfmodstudio) extra imports ---
  { "_ZdlPv", (uintptr_t)&_ZdlPv },                       // operator delete
  { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
  { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
  { "setpriority", (uintptr_t)&ret0 },
  { "fputs", (uintptr_t)&fputs_fake },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  // force trilinear texture filtering
  so_find_import(dynlib_functions, dynlib_numfunctions, "glTexParameteri")->func = (uintptr_t)glTexParameteriHook;
}

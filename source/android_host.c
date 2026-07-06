/* android_host.c -- Android NativeActivity / native_app_glue host
 *
 * See android_host.h. Implements the NDK C APIs the glue + engine import,
 * plus an in-process fake pipe so the glue's command channel works without a
 * real OS pipe (libnx has no usable pipe()/poll()).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "android_host.h"

// ALooper return / event constants
#define ALOOPER_POLL_WAKE    (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT (-3)
#define ALOOPER_POLL_ERROR   (-4)
#define ALOOPER_EVENT_INPUT  1

volatile int android_host_finish_requested = 0;

// ---------------------------------------------------------------------------
// fake fd registry: the glue's command pipe and input queue need fds the looper
// and libc understand. Handed out from a high base so they never collide with
// real newlib fds; imports.c routes the libc calls here.
// ---------------------------------------------------------------------------

#define FAKE_FD_BASE 0x40000000
#define MAX_FAKE_FDS 16

typedef enum { FFD_NONE, FFD_PIPE_R, FFD_PIPE_W, FFD_INPUTQ } FakeFdType;

typedef struct {
  FakeFdType type;
  void *obj;
} FakeFd;

static FakeFd g_fake_fds[MAX_FAKE_FDS];
static pthread_mutex_t g_fake_lock = PTHREAD_MUTEX_INITIALIZER;

static int fake_fd_alloc(FakeFdType type, void *obj) {
  pthread_mutex_lock(&g_fake_lock);
  for (int i = 0; i < MAX_FAKE_FDS; i++) {
    if (g_fake_fds[i].type == FFD_NONE) {
      g_fake_fds[i].type = type;
      g_fake_fds[i].obj = obj;
      pthread_mutex_unlock(&g_fake_lock);
      return FAKE_FD_BASE + i;
    }
  }
  pthread_mutex_unlock(&g_fake_lock);
  return -1;
}

static FakeFd *fake_fd_get(int fd) {
  if (fd < FAKE_FD_BASE || fd >= FAKE_FD_BASE + MAX_FAKE_FDS)
    return NULL;
  FakeFd *f = &g_fake_fds[fd - FAKE_FD_BASE];
  return f->type == FFD_NONE ? NULL : f;
}

// ---------------------------------------------------------------------------
// fake pipe (the glue's msgpipe). 1-byte command bytes only.
// ---------------------------------------------------------------------------

typedef struct {
  pthread_mutex_t mtx;
  uint8_t buf[256];
  int head, tail, count;
  ALooper *looper; // set when the read end is registered, so writes can wake
} FakePipe;

static ALooper *g_looper; // single global looper (one game thread)
static void looper_wake(ALooper *l);

int android_fake_fd_is_fake(int fd) {
  return fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FDS;
}

static int pipe_readable(FakePipe *p) {
  pthread_mutex_lock(&p->mtx);
  int n = p->count;
  pthread_mutex_unlock(&p->mtx);
  return n > 0;
}

// readability of a single fake fd
int android_fake_fd_readable(int fd) {
  FakeFd *f = fake_fd_get(fd);
  if (!f)
    return 0;
  if (f->type == FFD_PIPE_R)
    return pipe_readable((FakePipe *)f->obj);
  if (f->type == FFD_INPUTQ) {
    extern int android_inputq_count(void *q);
    return android_inputq_count(f->obj) > 0;
  }
  return 0;
}

int android_fake_fd_read(int fd, void *buf, size_t count) {
  FakeFd *f = fake_fd_get(fd);
  if (!f || f->type != FFD_PIPE_R) {
    errno = EBADF;
    return -1;
  }
  FakePipe *p = f->obj;
  pthread_mutex_lock(&p->mtx);
  size_t n = 0;
  uint8_t *out = buf;
  while (n < count && p->count > 0) {
    out[n++] = p->buf[p->head];
    p->head = (p->head + 1) % sizeof(p->buf);
    p->count--;
  }
  pthread_mutex_unlock(&p->mtx);
  if (n == 0) {
    errno = EAGAIN;
    return -1;
  }
  if (n == 1)
    debugPrintf("pipe READ cmd=%d\n", (int)*out);
  return (int)n;
}

int android_fake_fd_write(int fd, const void *buf, size_t count) {
  FakeFd *f = fake_fd_get(fd);
  if (!f || f->type != FFD_PIPE_W) {
    errno = EBADF;
    return -1;
  }
  FakePipe *p = f->obj;
  pthread_mutex_lock(&p->mtx);
  size_t n = 0;
  const uint8_t *in = buf;
  while (n < count && p->count < (int)sizeof(p->buf)) {
    p->buf[p->tail] = in[n++];
    p->tail = (p->tail + 1) % sizeof(p->buf);
    p->count++;
  }
  ALooper *l = p->looper;
  pthread_mutex_unlock(&p->mtx);
  if (count == 1)
    debugPrintf("pipe WRITE cmd=%d\n", (int)*(const uint8_t *)buf);
  if (l)
    looper_wake(l); // unblock a pollAll waiting on the read end
  return (int)n;
}

int android_fake_fd_close(int fd) {
  pthread_mutex_lock(&g_fake_lock);
  FakeFd *f = fake_fd_get(fd);
  if (f) {
    // leak the underlying object; these live for the process lifetime
    f->type = FFD_NONE;
    f->obj = NULL;
  }
  pthread_mutex_unlock(&g_fake_lock);
  return 0;
}

// the glue calls pipe() in android_app_create
int android_pipe_fake(int fds[2]) {
  FakePipe *p = calloc(1, sizeof(*p));
  pthread_mutex_init(&p->mtx, NULL);
  int rfd = fake_fd_alloc(FFD_PIPE_R, p);
  int wfd = fake_fd_alloc(FFD_PIPE_W, p);
  if (rfd < 0 || wfd < 0) {
    free(p);
    errno = EMFILE;
    return -1;
  }
  fds[0] = rfd;
  fds[1] = wfd;
  debugPrintf("android_host: fake pipe r=%d w=%d\n", rfd, wfd);
  return 0;
}

// ---------------------------------------------------------------------------
// ALooper
// ---------------------------------------------------------------------------

struct ALooper {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  struct {
    int fd;
    int ident;
    void *data;
    int enabled;
  } fds[16];
  int nfds;
  int woken; // set by ALooper_wake; makes pollOnce return ALOOPER_POLL_WAKE
};

// internal: nudge a blocked pollOnce to re-scan the fds (does NOT return WAKE)
static void looper_wake(ALooper *l) {
  pthread_mutex_lock(&l->mtx);
  pthread_cond_broadcast(&l->cond);
  pthread_mutex_unlock(&l->mtx);
}

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  if (!g_looper) {
    g_looper = calloc(1, sizeof(ALooper));
    pthread_mutex_init(&g_looper->mtx, NULL);
    pthread_cond_init(&g_looper->cond, NULL);
  }
  return g_looper;
}

// The engine calls ALooper_forThread() to fetch the looper already prepared for
// the current thread. We run a single game/loop thread, so hand back the one
// global looper (NULL before the glue prepares it, matching the NDK contract).
ALooper *ALooper_forThread(void) {
  return g_looper;
}

int ALooper_addFd(ALooper *looper, int fd, int ident, int events, void *callback, void *data) {
  (void)events; (void)callback;
  if (!looper)
    return -1;
  pthread_mutex_lock(&looper->mtx);
  // if a fake pipe read end is being added, remember the looper so writes wake
  FakeFd *f = fake_fd_get(fd);
  if (f && f->type == FFD_PIPE_R)
    ((FakePipe *)f->obj)->looper = looper;
  // replace existing registration for this fd
  for (int i = 0; i < looper->nfds; i++) {
    if (looper->fds[i].fd == fd) {
      looper->fds[i].ident = ident;
      looper->fds[i].data = data;
      looper->fds[i].enabled = 1;
      pthread_mutex_unlock(&looper->mtx);
      return 1;
    }
  }
  if (looper->nfds < 16) {
    looper->fds[looper->nfds].fd = fd;
    looper->fds[looper->nfds].ident = ident;
    looper->fds[looper->nfds].data = data;
    looper->fds[looper->nfds].enabled = 1;
    looper->nfds++;
  }
  pthread_mutex_unlock(&looper->mtx);
  return 1;
}

int ALooper_removeFd(ALooper *looper, int fd) {
  if (!looper)
    return -1;
  pthread_mutex_lock(&looper->mtx);
  for (int i = 0; i < looper->nfds; i++) {
    if (looper->fds[i].fd == fd)
      looper->fds[i].enabled = 0;
  }
  pthread_mutex_unlock(&looper->mtx);
  return 1;
}

// public NDK API: returns ALOOPER_POLL_WAKE (-1) to break the engine's
// `while (pollAll() >= 0)` command-drain loop so it proceeds to render
void ALooper_wake(ALooper *looper) {
  if (!looper)
    return;
  pthread_mutex_lock(&looper->mtx);
  looper->woken = 1;
  pthread_cond_broadcast(&looper->cond);
  pthread_mutex_unlock(&looper->mtx);
}

// scan registered fds for a ready one; returns its ident or -1
static int looper_scan(ALooper *l, int *outFd, int *outEvents, void **outData) {
  for (int i = 0; i < l->nfds; i++) {
    if (!l->fds[i].enabled)
      continue;
    if (android_fake_fd_readable(l->fds[i].fd)) {
      if (outFd) *outFd = l->fds[i].fd;
      if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
      if (outData) *outData = l->fds[i].data;
      return l->fds[i].ident;
    }
  }
  return -1;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  ALooper *l = g_looper;
  if (!l)
    return ALOOPER_POLL_ERROR;

  pthread_mutex_lock(&l->mtx);
  for (;;) {
    // an explicit ALooper_wake takes priority and returns WAKE (-1)
    if (l->woken) {
      l->woken = 0;
      pthread_mutex_unlock(&l->mtx);
      return ALOOPER_POLL_WAKE;
    }
    int ident = looper_scan(l, outFd, outEvents, outData);
    if (ident >= 0) {
      pthread_mutex_unlock(&l->mtx);
      return ident;
    }
    if (timeoutMillis == 0) {
      pthread_mutex_unlock(&l->mtx);
      return ALOOPER_POLL_TIMEOUT;
    }
    if (timeoutMillis < 0) {
      pthread_cond_wait(&l->cond, &l->mtx);
    } else {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeoutMillis / 1000;
      ts.tv_nsec += (long)(timeoutMillis % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      int rc = pthread_cond_timedwait(&l->cond, &l->mtx, &ts);
      if (rc == ETIMEDOUT) {
        if (l->woken) { l->woken = 0; pthread_mutex_unlock(&l->mtx); return ALOOPER_POLL_WAKE; }
        int id = looper_scan(l, outFd, outEvents, outData);
        pthread_mutex_unlock(&l->mtx);
        return id >= 0 ? id : ALOOPER_POLL_TIMEOUT;
      }
    }
  }
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  // we never use callback-based fds, so pollAll == pollOnce
  return ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
}

// ---------------------------------------------------------------------------
// AInputQueue + event model
// ---------------------------------------------------------------------------

struct AInputQueue {
  pthread_mutex_t mtx;
  AInputEvent events[64];
  int head, tail, count;
  AInputEvent current; // copy handed out by getEvent until finishEvent
  int has_current;
  int fd;
  ALooper *looper;
  int ident;
};

static AInputQueue *g_input_queue;

int android_inputq_count(void *q) {
  AInputQueue *iq = q;
  pthread_mutex_lock(&iq->mtx);
  int n = iq->count;
  pthread_mutex_unlock(&iq->mtx);
  return n;
}

void android_host_init(void) {
  memset(g_fake_fds, 0, sizeof(g_fake_fds));
}

void android_host_create_input_queue(ANativeActivity *activity) {
  AInputQueue *iq = calloc(1, sizeof(*iq));
  pthread_mutex_init(&iq->mtx, NULL);
  iq->fd = fake_fd_alloc(FFD_INPUTQ, iq);
  g_input_queue = iq;
  debugPrintf("android_host: input queue fd=%d\n", iq->fd);
  if (activity->callbacks->onInputQueueCreated)
    activity->callbacks->onInputQueueCreated(activity, iq);
}

void android_host_post_input(const AInputEvent *ev) {
  AInputQueue *iq = g_input_queue;
  if (!iq)
    return;
  pthread_mutex_lock(&iq->mtx);
  if (iq->count < 64) {
    iq->events[iq->tail] = *ev;
    iq->tail = (iq->tail + 1) % 64;
    iq->count++;
  }
  ALooper *l = iq->looper;
  pthread_mutex_unlock(&iq->mtx);
  if (l)
    looper_wake(l);
}

int AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident, void *callback, void *data) {
  queue->looper = looper;
  queue->ident = ident;
  // forward the glue's `data` (its android_poll_source*), not `queue`: pollAll
  // hands it back as outData and the glue calls source->process()
  ALooper_addFd(looper, queue->fd, ident, ALOOPER_EVENT_INPUT, callback, data);
  return 0;
}

int AInputQueue_detachLooper(AInputQueue *queue) {
  if (queue->looper)
    ALooper_removeFd(queue->looper, queue->fd);
  queue->looper = NULL;
  return 0;
}

int AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
  pthread_mutex_lock(&queue->mtx);
  if (queue->count == 0) {
    pthread_mutex_unlock(&queue->mtx);
    return -1; // negative: no events
  }
  queue->current = queue->events[queue->head];
  queue->head = (queue->head + 1) % 64;
  queue->count--;
  queue->has_current = 1;
  pthread_mutex_unlock(&queue->mtx);
  *outEvent = &queue->current;
  return 0;
}

int AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event) {
  (void)queue; (void)event;
  return 0; // not pre-dispatched (no IME)
}

void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled) {
  (void)event; (void)handled;
  queue->has_current = 0;
}

// --- AInputEvent / AKeyEvent / AMotionEvent accessors ----------------------

int32_t AInputEvent_getType(const AInputEvent *e) { return e->type; }
int32_t AInputEvent_getSource(const AInputEvent *e) { return e->source; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e->deviceId; }

int32_t AKeyEvent_getAction(const AInputEvent *e) { return e->key_action; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { return e->key_code; }
int32_t AKeyEvent_getMetaState(const AInputEvent *e) { return e->meta_state; }
int32_t AKeyEvent_getFlags(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { (void)e; return 0; }
int64_t AKeyEvent_getEventTime(const AInputEvent *e) { (void)e; return 0; }
int64_t AKeyEvent_getDownTime(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getScanCode(const AInputEvent *e) { (void)e; return 0; }

int32_t AMotionEvent_getAction(const AInputEvent *e) { return e->motion_action; }
size_t  AMotionEvent_getPointerCount(const AInputEvent *e) { return e->pointer_count; }
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t idx) {
  return idx < HOST_MAX_POINTERS ? e->pointer_id[idx] : 0;
}
float AMotionEvent_getX(const AInputEvent *e, size_t idx) {
  return idx < HOST_MAX_POINTERS ? e->x[idx] : 0.0f;
}
float AMotionEvent_getY(const AInputEvent *e, size_t idx) {
  return idx < HOST_MAX_POINTERS ? e->y[idx] : 0.0f;
}
float AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t idx) {
  (void)idx;
  if (axis >= 0 && axis < 48)
    return e->axis[axis];
  return 0.0f;
}
int64_t AMotionEvent_getEventTime(const AInputEvent *e) { (void)e; return 0; }
float AMotionEvent_getPressure(const AInputEvent *e, size_t idx) { (void)e; (void)idx; return 1.0f; }

// ---------------------------------------------------------------------------
// ANativeWindow: EGL/mesa wants the libnx NWindow* directly, so ANativeWindow*
// IS the default NWindow*.
// ---------------------------------------------------------------------------

ANativeWindow *android_host_get_window(void) {
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  return (ANativeWindow *)win;
}

int32_t ANativeWindow_getWidth(ANativeWindow *window) {
  (void)window; debugPrintf("ANativeWindow_getWidth -> %d\n", screen_width); return screen_width;
}
int32_t ANativeWindow_getHeight(ANativeWindow *window) {
  (void)window; debugPrintf("ANativeWindow_getHeight -> %d\n", screen_height); return screen_height;
}
int32_t ANativeWindow_getFormat(ANativeWindow *window) { (void)window; return 1; /* RGBA_8888 */ }

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t w, int32_t h, int32_t format) {
  (void)format;
  // (0,0) means "use the native default size" -- give it our render resolution
  // rather than leaving the window unsized (which yields a 0x0 surface -> black).
  if (w <= 0 || h <= 0) { w = screen_width; h = screen_height; }
  debugPrintf("ANativeWindow_setBuffersGeometry(%d, %d)\n", w, h);
  nwindowSetDimensions((NWindow *)window, w, h);
  return 0;
}

void ANativeWindow_acquire(ANativeWindow *window) { (void)window; }
void ANativeWindow_release(ANativeWindow *window) { (void)window; }

// ---------------------------------------------------------------------------
// ANativeActivity APIs the engine calls
// ---------------------------------------------------------------------------

void ANativeActivity_finish(ANativeActivity *activity) {
  (void)activity;
  debugPrintf("ANativeActivity_finish requested\n");
  android_host_finish_requested = 1;
}

void ANativeActivity_setWindowFlags(ANativeActivity *activity, uint32_t addFlags, uint32_t removeFlags) {
  (void)activity; (void)addFlags; (void)removeFlags;
}

void ANativeActivity_showSoftInput(ANativeActivity *activity, uint32_t flags) { (void)activity; (void)flags; }
void ANativeActivity_hideSoftInput(ANativeActivity *activity, uint32_t flags) { (void)activity; (void)flags; }

// The engine forces landscape via AConfiguration_setOrientation; we are always
// landscape on the Switch, so accept and ignore it.
void AConfiguration_setOrientation(AConfiguration *c, int32_t orientation) {
  (void)c; (void)orientation;
}

// ---------------------------------------------------------------------------
// AConfiguration: hand back sane fixed values
// ---------------------------------------------------------------------------

struct AConfiguration { int dummy; };

AConfiguration *AConfiguration_new(void) {
  return calloc(1, sizeof(AConfiguration));
}
void AConfiguration_delete(AConfiguration *c) { free(c); }
void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *m) { (void)c; (void)m; }

int32_t AConfiguration_getMcc(AConfiguration *c) { (void)c; return 0; }
int32_t AConfiguration_getMnc(AConfiguration *c) { (void)c; return 0; }
// The engine picks its initial locale from this 2-letter code (falls back to
// English if it has no assets for it); the in-game menu overrides it afterwards.
// Follows the Switch system language.
void AConfiguration_getLanguage(AConfiguration *c, char *out) {
  (void)c;
  static char lang[2];
  static int done = 0;
  if (!done) {
    done = 1;
    u64 code = 0;
    if (R_SUCCEEDED(setInitialize())) {
      setGetSystemLanguage(&code);
      setExit();
    }
    lang[0] = code ? (char)(code & 0xff)        : 'e';
    lang[1] = code ? (char)((code >> 8) & 0xff) : 'n';
  }
  out[0] = lang[0];
  out[1] = lang[1];
}
void    AConfiguration_getCountry(AConfiguration *c, char *out) { (void)c; out[0]='U'; out[1]='S'; }
int32_t AConfiguration_getOrientation(AConfiguration *c) { (void)c; return 2; /* LAND */ }
int32_t AConfiguration_getTouchscreen(AConfiguration *c) { (void)c; return 3; /* FINGER */ }
int32_t AConfiguration_getDensity(AConfiguration *c) { (void)c; return 320; }
int32_t AConfiguration_getKeyboard(AConfiguration *c) { (void)c; return 1; }
int32_t AConfiguration_getNavigation(AConfiguration *c) { (void)c; return 1; }
int32_t AConfiguration_getKeysHidden(AConfiguration *c) { (void)c; return 1; }
int32_t AConfiguration_getNavHidden(AConfiguration *c) { (void)c; return 1; }
int32_t AConfiguration_getSdkVersion(AConfiguration *c) { (void)c; return 30; }
int32_t AConfiguration_getScreenSize(AConfiguration *c) { (void)c; return 4; /* XLARGE */ }
int32_t AConfiguration_getScreenLong(AConfiguration *c) { (void)c; return 2; /* YES */ }
int32_t AConfiguration_getUiModeType(AConfiguration *c) { (void)c; return 1; }
int32_t AConfiguration_getUiModeNight(AConfiguration *c) { (void)c; return 1; }

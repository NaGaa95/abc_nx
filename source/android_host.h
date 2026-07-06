/* android_host.h -- minimal Android NativeActivity / native_app_glue host
 *
 * Castle of Illusion runs as a raw NDK NativeActivity driven by the standard
 * android_native_app_glue (compiled into libViewer_GP.so). This module is the
 * "Android framework" side: it provides the NDK C APIs the glue + engine
 * import (ALooper, AInputQueue, ANativeWindow, AConfiguration, ANativeActivity)
 * plus a fake in-process pipe so the glue's command channel works without a
 * real OS pipe/poll. main.c builds a fake ANativeActivity, calls
 * ANativeActivity_onCreate(), and drives the lifecycle/input from here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __ANDROID_HOST_H__
#define __ANDROID_HOST_H__

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// ANativeActivity / callbacks (NDK layout; offsets verified against the binary)
// ---------------------------------------------------------------------------

typedef struct ANativeActivity ANativeActivity;
typedef struct AInputQueue AInputQueue;
typedef struct ALooper ALooper;
typedef struct ANativeWindow ANativeWindow;
typedef struct AConfiguration AConfiguration;
typedef struct AInputEvent AInputEvent;
typedef struct AAssetManager AAssetManager;

typedef struct ANativeActivityCallbacks {
  void (*onStart)(ANativeActivity *activity);
  void (*onResume)(ANativeActivity *activity);
  void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outSize);
  void (*onPause)(ANativeActivity *activity);
  void (*onStop)(ANativeActivity *activity);
  void (*onDestroy)(ANativeActivity *activity);
  void (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
  void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
  void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
  void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
  void (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
  void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
  void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
  void (*onContentRectChanged)(ANativeActivity *activity, const void *rect);
  void (*onConfigurationChanged)(ANativeActivity *activity);
  void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks; // 0
  void *vm;                            // 8  JavaVM*
  void *env;                           // 16 JNIEnv*
  void *clazz;                         // 24 jobject
  const char *internalDataPath;        // 32
  const char *externalDataPath;        // 40
  int32_t sdkVersion;                  // 48
  void *instance;                      // 56 android_app* (set by glue)
  AAssetManager *assetManager;         // 64
  const char *obbPath;                 // 72
};

// ---------------------------------------------------------------------------
// input event model (we fully own the AInputEvent representation: the engine
// only ever reads it back through the AInputEvent/AKeyEvent/AMotionEvent
// accessors below, so the struct layout is ours to define)
// ---------------------------------------------------------------------------

#define AINPUT_EVENT_TYPE_KEY    1
#define AINPUT_EVENT_TYPE_MOTION 2

#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP   1

// motion actions (masked; pointer index in the high bits for pointer up/down)
#define AMOTION_EVENT_ACTION_MASK            0xff
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8
#define AMOTION_EVENT_ACTION_DOWN            0
#define AMOTION_EVENT_ACTION_UP              1
#define AMOTION_EVENT_ACTION_MOVE            2
#define AMOTION_EVENT_ACTION_CANCEL          3
#define AMOTION_EVENT_ACTION_POINTER_DOWN    5
#define AMOTION_EVENT_ACTION_POINTER_UP      6

#define AINPUT_SOURCE_TOUCHSCREEN 0x1002
#define AINPUT_SOURCE_JOYSTICK    0x1000010
#define AINPUT_SOURCE_GAMEPAD     0x401

#define HOST_MAX_POINTERS 8

struct AInputEvent {
  int32_t type;     // AINPUT_EVENT_TYPE_*
  int32_t source;
  int32_t deviceId;
  // key
  int32_t key_action;
  int32_t key_code;
  int32_t meta_state;
  // motion
  int32_t motion_action;
  int32_t pointer_count;
  int32_t pointer_id[HOST_MAX_POINTERS];
  float x[HOST_MAX_POINTERS];
  float y[HOST_MAX_POINTERS];
  // joystick axes, indexed by Android AMOTION_EVENT_AXIS_* (we use 0..47)
  float axis[48];
};

// ---------------------------------------------------------------------------
// host lifecycle / input injection (called from main.c, the framework thread)
// ---------------------------------------------------------------------------

void android_host_init(void);

// build & register the single fake input queue, hand it to the activity
void android_host_create_input_queue(ANativeActivity *activity);

// push a fully-formed event into the input queue and wake the game's looper
void android_host_post_input(const AInputEvent *ev);

// the game called ANativeActivity_finish(): set when it wants to quit
extern volatile int android_host_finish_requested;

// current ANativeWindow handed to the engine (wraps the libnx default NWindow)
ANativeWindow *android_host_get_window(void);

// fake fd plumbing for the glue's command pipe (imports.c routes libc here)
int android_pipe_fake(int fds[2]);
int android_fake_fd_is_fake(int fd);
int android_fake_fd_read(int fd, void *buf, size_t count);
int android_fake_fd_write(int fd, const void *buf, size_t count);
int android_fake_fd_close(int fd);

// ---------------------------------------------------------------------------
// NDK C APIs implemented in android_host.c (referenced by imports.c)
// ---------------------------------------------------------------------------

ALooper *ALooper_prepare(int opts);
ALooper *ALooper_forThread(void);
int  ALooper_addFd(ALooper *looper, int fd, int ident, int events, void *callback, void *data);
int  ALooper_removeFd(ALooper *looper, int fd);
void ALooper_wake(ALooper *looper);
int  ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData);
int  ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData);

int  AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident, void *callback, void *data);
int  AInputQueue_detachLooper(AInputQueue *queue);
int  AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent);
int  AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event);
void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled);

int32_t AInputEvent_getType(const AInputEvent *e);
int32_t AInputEvent_getSource(const AInputEvent *e);
int32_t AInputEvent_getDeviceId(const AInputEvent *e);
int32_t AKeyEvent_getAction(const AInputEvent *e);
int32_t AKeyEvent_getKeyCode(const AInputEvent *e);
int32_t AKeyEvent_getMetaState(const AInputEvent *e);
int32_t AKeyEvent_getFlags(const AInputEvent *e);
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e);
int64_t AKeyEvent_getEventTime(const AInputEvent *e);
int64_t AKeyEvent_getDownTime(const AInputEvent *e);
int32_t AKeyEvent_getScanCode(const AInputEvent *e);
int32_t AMotionEvent_getAction(const AInputEvent *e);
size_t  AMotionEvent_getPointerCount(const AInputEvent *e);
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t idx);
float   AMotionEvent_getX(const AInputEvent *e, size_t idx);
float   AMotionEvent_getY(const AInputEvent *e, size_t idx);
float   AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t idx);
int64_t AMotionEvent_getEventTime(const AInputEvent *e);
float   AMotionEvent_getPressure(const AInputEvent *e, size_t idx);

int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_getFormat(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t w, int32_t h, int32_t format);
void    ANativeWindow_acquire(ANativeWindow *window);
void    ANativeWindow_release(ANativeWindow *window);

void ANativeActivity_finish(ANativeActivity *activity);
void ANativeActivity_setWindowFlags(ANativeActivity *activity, uint32_t addFlags, uint32_t removeFlags);
void ANativeActivity_showSoftInput(ANativeActivity *activity, uint32_t flags);
void ANativeActivity_hideSoftInput(ANativeActivity *activity, uint32_t flags);

AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(AConfiguration *c);
void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *m);
void AConfiguration_setOrientation(AConfiguration *c, int32_t orientation);
int32_t AConfiguration_getMcc(AConfiguration *c);
int32_t AConfiguration_getMnc(AConfiguration *c);
void    AConfiguration_getLanguage(AConfiguration *c, char *out);
void    AConfiguration_getCountry(AConfiguration *c, char *out);
int32_t AConfiguration_getOrientation(AConfiguration *c);
int32_t AConfiguration_getTouchscreen(AConfiguration *c);
int32_t AConfiguration_getDensity(AConfiguration *c);
int32_t AConfiguration_getKeyboard(AConfiguration *c);
int32_t AConfiguration_getNavigation(AConfiguration *c);
int32_t AConfiguration_getKeysHidden(AConfiguration *c);
int32_t AConfiguration_getNavHidden(AConfiguration *c);
int32_t AConfiguration_getSdkVersion(AConfiguration *c);
int32_t AConfiguration_getScreenSize(AConfiguration *c);
int32_t AConfiguration_getScreenLong(AConfiguration *c);
int32_t AConfiguration_getUiModeType(AConfiguration *c);
int32_t AConfiguration_getUiModeNight(AConfiguration *c);

#endif

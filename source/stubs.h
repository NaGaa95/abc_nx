/* stubs.h -- extra bionic/NDK shims specific to After Burner Climax
 *
 * libacb.so imports a wider surface than Castle of Illusion: Android sensors
 * (accelerometer), AAssetDir enumeration, an anonymous mmap allocator, and a
 * pile of POSIX odds and ends (signals, termios, process, networking) that the
 * engine references but does not need on a console. These are implemented or
 * safely stubbed here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __STUBS_H__
#define __STUBS_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <setjmp.h>
#include <time.h>

// --- real sleep (newlib lacks nanosleep on Switch) -------------------------
int nanosleep_fake(const struct timespec *req, struct timespec *rem);

// --- sigsetjmp/siglongjmp over newlib's plain setjmp -----------------------
int  sigsetjmp_fake(jmp_buf env, int savesigs);
void siglongjmp_fake(jmp_buf env, int val);

// --- anonymous mmap allocator (no file-backed mappings) --------------------
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap_fake(void *addr, size_t length);
int   mprotect_fake(void *addr, size_t length, int prot);
int   madvise_fake(void *addr, size_t length, int advice);
int   mlock_fake(const void *addr, size_t length);

// --- Android sensors: report "no sensors" so the engine uses the gamepad ---
void *ASensorManager_getInstance_fake(void);
void *ASensorManager_getDefaultSensor_fake(void *mgr, int type);
void *ASensorManager_createEventQueue_fake(void *mgr, void *looper, int ident, void *cb, void *data);
int   ASensorEventQueue_enableSensor_fake(void *q, void *sensor);
int   ASensorEventQueue_disableSensor_fake(void *q, void *sensor);
int   ASensorEventQueue_setEventRate_fake(void *q, void *sensor, int usec);
int   ASensorEventQueue_getEvents_fake(void *q, void *events, size_t count);

// --- AAssetManager directory enumeration over the real assets/ folder ------
void       *AAssetManager_openDir_fake(void *mgr, const char *dirName);
const char *AAssetDir_getNextFileName_fake(void *assetDir);
void        AAssetDir_close_fake(void *assetDir);
void        AAssetDir_rewind_fake(void *assetDir);

// --- fake-fd aware raw read/write (the glue command pipe uses fake fds) -----
ssize_t write_fake(int fd, const void *buf, size_t count);
ssize_t read_fake(int fd, void *buf, size_t count);

// --- POSIX odds and ends the engine references but never needs on console --
int   poll_fake(void *fds, unsigned long nfds, int timeout);
int   ioctl_fake(int fd, unsigned long request, ...);
int   sigaction_fake(int sig, const void *act, void *oldact);
void *signal_fake(int sig, void *handler);
int   ret0_sig(void); // sigemptyset/sigfillset/sigdelset/sigprocmask family

int   fork_fake(void);
int   kill_fake(int pid, int sig);
int   system_fake(const char *cmd);
int   getrlimit_fake(int resource, void *rlim);
int   sched_yield_fake(void);
void *getpwuid_fake(unsigned uid);
unsigned getuid_fake(void);

// --- networking: offline (name resolution fails, sockets already stubbed) --
int         getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res);
void        freeaddrinfo_fake(void *res);
const char *gai_strerror_fake(int errcode);
int         getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags);

#endif

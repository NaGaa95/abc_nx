/* opensl.c -- minimal fake libOpenSLES.so backed by libnx audout.
 *
 * FMOD Ex drives an AndroidSimpleBufferQueue player: it mixes everything and
 * pushes one interleaved stereo PCM stream through the queue. We implement just
 * enough of the OpenSL ES 1.0.1 object model to create that player and pump its
 * buffers into libnx audout. imports.c routes the dlopen/dlsym here.
 */

#include <switch.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include "util.h"

// ---------------------------------------------------------------------------
// OpenSL ES ABI (we declare the slice we need rather than pull Android headers)
// ---------------------------------------------------------------------------

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_FEATURE_UNSUPPORTED  12
#define SL_RESULT_PARAMETER_INVALID    13

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_DATAFORMAT_PCM 2

// every OpenSL object/interface handle is a pointer to a pointer to a vtable;
// we store a back-pointer to the owning object right after the vtable pointer.
typedef struct {
  const void *vtable;  // points at one of the static *_Vtable structs below
  void       *parent;  // owning Engine/OutputMix/Player
} Itf;

// SLDataFormat_PCM as laid out by the caller (FMOD)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec; // milliHz (48000 Hz == 48000000)
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;

// interface IDs: FMOD dlsym's our SL_IID_* pointers and passes them straight
// back to GetInterface, so identity (pointer compare) is all that matters.
static const int IID_ENGINE_v = 0, IID_PLAY_v = 0, IID_BUFFERQUEUE_v = 0,
                 IID_RECORD_v = 0, IID_CONFIG_v = 0;
static const void *IID_ENGINE      = &IID_ENGINE_v;
static const void *IID_PLAY        = &IID_PLAY_v;
static const void *IID_BUFFERQUEUE = &IID_BUFFERQUEUE_v;
static const void *IID_RECORD      = &IID_RECORD_v;
static const void *IID_CONFIG      = &IID_CONFIG_v;

// ---------------------------------------------------------------------------
// audout-backed buffer-queue player
// ---------------------------------------------------------------------------

#define BQ_RING     32              // enqueued-but-not-yet-played PCM buffers
#define POOL_BUFS   4               // audout double/quad buffering
#define POOL_BYTES  (32 * 1024)     // per audout buffer (>= one resampled block)

typedef void (*BQCallback)(void *bqItf, void *context);

typedef struct Player {
  Itf obj, play, bq, cfg;           // the four GetInterface'able interfaces

  int rate, channels;               // source PCM format (from CreateAudioPlayer)
  SLuint32 playstate;

  BQCallback cb;                    // FMOD's "buffer consumed, give me the next"
  void      *cb_ctx;

  // ring of enqueued (FMOD-owned) PCM buffers awaiting playback
  struct { const void *data; SLuint32 size; } ring[BQ_RING];
  int       r_head, r_tail, r_count;
  Mutex     r_lock;
  CondVar   r_cond;

  Thread    pump;
  bool      pump_run, started;

  // audout output pool
  bool             audout_ok;
  AudioOutBuffer   pool[POOL_BUFS];
  void            *pool_mem[POOL_BUFS];
  bool             busy[POOL_BUFS];   // appended, not yet reclaimed
} Player;

static bool g_audout_inited = false;

// linear-resample interleaved stereo s16 from in_rate -> out_rate(48000).
// returns bytes written to dst. mono is upmixed to stereo.
static SLuint32 resample_to_48k(const int16_t *src, SLuint32 src_bytes,
                                int channels, int in_rate,
                                int16_t *dst, SLuint32 dst_cap) {
  const int in_frames = src_bytes / (2 * channels);
  if (in_frames <= 0) return 0;
  const int out_rate = 48000;
  if (in_rate == out_rate && channels == 2) {
    SLuint32 n = src_bytes > dst_cap ? dst_cap : src_bytes;
    memcpy(dst, src, n);
    return n;
  }
  // out_frames = in_frames * out_rate / in_rate
  long out_frames = (long)in_frames * out_rate / in_rate;
  const long cap_frames = dst_cap / 4;
  if (out_frames > cap_frames) out_frames = cap_frames;
  for (long i = 0; i < out_frames; i++) {
    // source position in fixed point
    long pos = i * in_rate / out_rate;
    if (pos >= in_frames) pos = in_frames - 1;
    int16_t l, r;
    if (channels == 2) { l = src[pos * 2]; r = src[pos * 2 + 1]; }
    else               { l = r = src[pos]; }
    dst[i * 2]     = l;
    dst[i * 2 + 1] = r;
  }
  return (SLuint32)(out_frames * 4);
}

static void mark_free(Player *p, AudioOutBuffer *rb) {
  for (int i = 0; i < POOL_BUFS; i++)
    if (&p->pool[i] == rb) { p->busy[i] = false; return; }
}

// get a pool buffer that isn't in flight, blocking on the audio device only
// when every buffer is still playing (which paces us to the output rate).
static AudioOutBuffer *get_free_buffer(Player *p) {
  AudioOutBuffer *rb = NULL; u32 c = 0;
  if (audoutGetReleasedAudioOutBuffer(&rb, &c) == 0 && rb) mark_free(p, rb);
  for (int i = 0; i < POOL_BUFS; i++)
    if (!p->busy[i]) return &p->pool[i];
  rb = NULL; c = 0;
  audoutWaitPlayFinish(&rb, &c, UINT64_MAX);
  if (rb) { mark_free(p, rb); return rb; }
  return &p->pool[0];
}

static void pump_thread(void *arg) {
  Player *p = (Player *)arg;

  while (p->pump_run) {
    // wait for an enqueued buffer / for playback to be running
    mutexLock(&p->r_lock);
    while (p->pump_run && (p->r_count == 0 || p->playstate != SL_PLAYSTATE_PLAYING))
      condvarWaitTimeout(&p->r_cond, &p->r_lock, 50000000ULL); // 50ms guard
    if (!p->pump_run) { mutexUnlock(&p->r_lock); break; }
    if (p->r_count == 0 || p->playstate != SL_PLAYSTATE_PLAYING) {
      mutexUnlock(&p->r_lock);
      continue;
    }
    const void *data = p->ring[p->r_head].data;
    SLuint32     size = p->ring[p->r_head].size;
    p->r_head = (p->r_head + 1) % BQ_RING;
    p->r_count--;
    mutexUnlock(&p->r_lock);

    if (p->audout_ok) {
      AudioOutBuffer *ab = get_free_buffer(p);
      SLuint32 n = resample_to_48k((const int16_t *)data, size, p->channels,
                                   p->rate, (int16_t *)ab->buffer, POOL_BYTES);
      ab->data_size = n;
      // SEGA intro logo audio (fmv.c) overrides the FMOD mix while it plays
      extern int fmv_audio_override(void *stream, int len_bytes);
      fmv_audio_override(ab->buffer, (int)n);
      audoutAppendAudioOutBuffer(ab);
      for (int i = 0; i < POOL_BUFS; i++)
        if (&p->pool[i] == ab) { p->busy[i] = true; break; }
    } else {
      // no audio device: pace to the buffer's duration so FMOD's mixer isn't spun at 100%
      int frames = size / (2 * (p->channels ? p->channels : 2));
      if (frames > 0) svcSleepThread((u64)frames * 1000000000ULL / (p->rate ? p->rate : 48000));
    }

    // tell FMOD this buffer is done -> it enqueues the next one
    if (p->cb) p->cb(&p->bq, p->cb_ctx);
  }
}

static void player_start(Player *p) {
  if (p->started) return;
  p->started = true;

  if (!g_audout_inited) {
    if (R_SUCCEEDED(audoutInitialize())) { g_audout_inited = true; }
  }
  if (g_audout_inited && R_SUCCEEDED(audoutStartAudioOut())) {
    p->audout_ok = true;
    for (int i = 0; i < POOL_BUFS; i++) {
      p->pool_mem[i] = memalign(0x1000, POOL_BYTES);
      memset(&p->pool[i], 0, sizeof(AudioOutBuffer));
      p->pool[i].buffer      = p->pool_mem[i];
      p->pool[i].buffer_size = POOL_BYTES;
      p->busy[i] = false;             // all free; pump fills them on demand
    }
    debugPrintf("OpenSL: audout started (%lu Hz %u ch) src=%dHz\n",
                (unsigned long)audoutGetSampleRate(), audoutGetChannelCount(), p->rate);
  } else {
    debugPrintf("OpenSL: audout unavailable -- running silent (paced)\n");
  }

  p->pump_run = true;
  // priority just above the main thread (0x2C) to avoid underrun; -2 = any core
  if (R_FAILED(threadCreate(&p->pump, pump_thread, p, NULL, 0x8000, 0x2B, -2))) {
    debugPrintf("OpenSL: pump threadCreate failed -- no audio\n");
    p->pump_run = false;
    return;
  }
  threadStart(&p->pump);
}

// ---------------------------------------------------------------------------
// SLAndroidSimpleBufferQueueItf
// ---------------------------------------------------------------------------

static SLresult bq_Enqueue(void *self, const void *buffer, SLuint32 size) {
  Player *p = ((Itf *)self)->parent;
  mutexLock(&p->r_lock);
  if (p->r_count >= BQ_RING) { mutexUnlock(&p->r_lock); return SL_RESULT_PARAMETER_INVALID; }
  p->ring[p->r_tail].data = buffer;
  p->ring[p->r_tail].size = size;
  p->r_tail = (p->r_tail + 1) % BQ_RING;
  p->r_count++;
  condvarWakeOne(&p->r_cond);
  mutexUnlock(&p->r_lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = ((Itf *)self)->parent;
  mutexLock(&p->r_lock);
  p->r_head = p->r_tail = p->r_count = 0;
  mutexUnlock(&p->r_lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *state) {
  Player *p = ((Itf *)self)->parent;
  if (state) { ((SLuint32 *)state)[0] = p->r_count; ((SLuint32 *)state)[1] = 0; }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, void *cb, void *ctx) {
  Player *p = ((Itf *)self)->parent;
  p->cb = (BQCallback)cb;
  p->cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}

static const void *BufferQueue_Vtable[] = {
  (void *)bq_Enqueue, (void *)bq_Clear, (void *)bq_GetState, (void *)bq_RegisterCallback,
};

// ---------------------------------------------------------------------------
// SLPlayItf  (SetPlayState/GetPlayState used; rest stubbed in slot order)
// ---------------------------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = ((Itf *)self)->parent;
  p->playstate = state;
  if (state == SL_PLAYSTATE_PLAYING) {
    player_start(p);
    mutexLock(&p->r_lock); condvarWakeOne(&p->r_cond); mutexUnlock(&p->r_lock);
  }
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *state) {
  Player *p = ((Itf *)self)->parent;
  if (state) *state = p->playstate;
  return SL_RESULT_SUCCESS;
}
static SLresult sl_ok(void) { return SL_RESULT_SUCCESS; }

static const void *Play_Vtable[] = {
  (void *)play_SetPlayState,   // SetPlayState
  (void *)play_GetPlayState,   // GetPlayState
  (void *)sl_ok,               // GetDuration
  (void *)sl_ok,               // GetPosition
  (void *)sl_ok,               // RegisterCallback
  (void *)sl_ok,               // SetCallbackEventsMask
  (void *)sl_ok,               // GetCallbackEventsMask
  (void *)sl_ok,               // SetMarkerPosition
  (void *)sl_ok,               // ClearMarkerPosition
  (void *)sl_ok,               // GetMarkerPosition
  (void *)sl_ok,               // SetPositionUpdatePeriod
  (void *)sl_ok,               // GetPositionUpdatePeriod
};

// ---------------------------------------------------------------------------
// SLAndroidConfigurationItf (FMOD sets stream type; no-op for us)
// ---------------------------------------------------------------------------

static const void *Config_Vtable[] = {
  (void *)sl_ok,  // SetConfiguration
  (void *)sl_ok,  // GetConfiguration
};

// ---------------------------------------------------------------------------
// SLObjectItf (Realize/GetInterface/Destroy). One vtable reused by every object.
// ---------------------------------------------------------------------------

typedef struct Engine    { Itf obj, engine; } Engine;
typedef struct OutputMix { Itf obj; } OutputMix;

// forward decls for object GetInterface dispatch
static SLresult obj_GetInterface(void *self, const void *iid, void *pItf);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async)  { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *s)    { (void)self; if (s) *s = 2 /*REALIZED*/; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static void     obj_AbortAsyncOperation(void *self) { (void)self; }
static void     obj_Destroy(void *self) { (void)self; /* leak: engine/mix/player live for process lifetime */ }
static SLresult obj_SetPriority(void *self, SLint32 p, SLboolean pre) { (void)self; (void)p; (void)pre; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLossOfControlInterfaces(void *self, SLint32 n, void *ids, SLboolean e) { (void)self; (void)n; (void)ids; (void)e; return SL_RESULT_SUCCESS; }

static const void *Object_Vtable[] = {
  (void *)obj_Realize,
  (void *)obj_Resume,
  (void *)obj_GetState,
  (void *)obj_GetInterface,
  (void *)obj_RegisterCallback,
  (void *)obj_AbortAsyncOperation,
  (void *)obj_Destroy,
  (void *)obj_SetPriority,
  (void *)obj_GetPriority,
  (void *)obj_SetLossOfControlInterfaces,
};

// ---------------------------------------------------------------------------
// SLEngineItf -- CreateOutputMix + CreateAudioPlayer are the ones FMOD calls
// ---------------------------------------------------------------------------

static SLresult eng_CreateAudioPlayer(void *self, void **pPlayer,
                                      SLDataSource *src, SLDataSink *snk,
                                      SLuint32 n, const void **ids, const SLboolean *req) {
  (void)self; (void)snk; (void)n; (void)ids; (void)req;
  Player *p = (Player *)calloc(1, sizeof(Player));
  if (!p) return SL_RESULT_PARAMETER_INVALID;
  p->obj.vtable  = Object_Vtable;     p->obj.parent  = p;
  p->play.vtable = Play_Vtable;       p->play.parent = p;
  p->bq.vtable   = BufferQueue_Vtable;p->bq.parent   = p;
  p->cfg.vtable  = Config_Vtable;     p->cfg.parent  = p;
  p->rate = 48000; p->channels = 2; p->playstate = SL_PLAYSTATE_STOPPED;
  mutexInit(&p->r_lock); condvarInit(&p->r_cond);

  if (src && src->pFormat) {
    SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)src->pFormat;
    if (fmt->formatType == SL_DATAFORMAT_PCM) {
      p->channels = fmt->numChannels ? fmt->numChannels : 2;
      p->rate     = fmt->samplesPerSec / 1000; // milliHz -> Hz
      if (p->rate < 8000 || p->rate > 192000) p->rate = 48000;
    }
  }
  debugPrintf("OpenSL: CreateAudioPlayer src=%d Hz %d ch\n", p->rate, p->channels);
  *pPlayer = &p->obj;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, void **pMix, SLuint32 n,
                                    const void **ids, const SLboolean *req) {
  (void)self; (void)n; (void)ids; (void)req;
  OutputMix *m = (OutputMix *)calloc(1, sizeof(OutputMix));
  if (!m) return SL_RESULT_PARAMETER_INVALID;
  m->obj.vtable = Object_Vtable; m->obj.parent = m;
  *pMix = &m->obj;
  return SL_RESULT_SUCCESS;
}

// ABI: exact OpenSL ES 1.0.1 method order; FMOD calls these by vtable index, so
// every slot (incl. the unused ones) must be present or CreateOutputMix shifts.
static const void *Engine_Vtable[] = {
  (void *)sl_ok,                    // 0  CreateLEDDevice
  (void *)sl_ok,                    // 1  CreateVibraDevice
  (void *)eng_CreateAudioPlayer,    // 2  CreateAudioPlayer
  (void *)sl_ok,                    // 3  CreateAudioRecorder
  (void *)sl_ok,                    // 4  CreateMidiPlayer
  (void *)sl_ok,                    // 5  CreateListener
  (void *)sl_ok,                    // 6  CreateMetadataExtractor
  (void *)eng_CreateOutputMix,      // 7  CreateOutputMix
  (void *)sl_ok,                    // 8  CreateExtensionObject
  (void *)sl_ok,                    // 9  QueryNumSupportedInterfaces
  (void *)sl_ok,                    // 10 QuerySupportedInterfaces
  (void *)sl_ok,                    // 11 QueryNumSupportedExtensions
  (void *)sl_ok,                    // 12 QuerySupportedExtension
  (void *)sl_ok,                    // 13 IsExtensionSupported
};

// GetInterface: return the sub-interface for the requested IID. Every object
// shares Object_Vtable, so dispatch by IID: Engine answers ENGINE, a Player
// answers PLAY/BUFFERQUEUE/CONFIG.
static SLresult obj_GetInterface(void *self, const void *iid, void *pItf) {
  Itf *base = (Itf *)self;
  void *parent = base->parent;
  if (iid == IID_ENGINE) {
    Engine *e = (Engine *)parent;
    *(void **)pItf = &e->engine;
    return SL_RESULT_SUCCESS;
  }
  Player *p = (Player *)parent;
  if (iid == IID_PLAY)        { *(void **)pItf = &p->play; return SL_RESULT_SUCCESS; }
  if (iid == IID_BUFFERQUEUE) { *(void **)pItf = &p->bq;   return SL_RESULT_SUCCESS; }
  if (iid == IID_CONFIG)      { *(void **)pItf = &p->cfg;  return SL_RESULT_SUCCESS; }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// slCreateEngine + dlsym table
// ---------------------------------------------------------------------------

static SLresult slCreateEngine(void **pEngine, SLuint32 numOpts, const void *opts,
                               SLuint32 numItf, const void **itfIds, const SLboolean *itfReq) {
  (void)numOpts; (void)opts; (void)numItf; (void)itfIds; (void)itfReq;
  Engine *e = (Engine *)calloc(1, sizeof(Engine));
  if (!e) return SL_RESULT_PARAMETER_INVALID;
  e->obj.vtable    = Object_Vtable; e->obj.parent    = e;
  e->engine.vtable = Engine_Vtable; e->engine.parent = e;
  *pEngine = &e->obj;
  debugPrintf("OpenSL: slCreateEngine -> %p\n", (void *)&e->obj);
  return SL_RESULT_SUCCESS;
}

void *opensl_dlsym(const char *name) {
  if (!name) return NULL;
  if (!strcmp(name, "slCreateEngine"))                  return (void *)&slCreateEngine;
  // OpenSL IID exports are pointers-to-pointers: FMOD reads *symbol, so return
  // the address of our IID pointer variable.
  if (!strcmp(name, "SL_IID_ENGINE"))                   return (void *)&IID_ENGINE;
  if (!strcmp(name, "SL_IID_PLAY"))                     return (void *)&IID_PLAY;
  if (!strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE")) return (void *)&IID_BUFFERQUEUE;
  if (!strcmp(name, "SL_IID_ANDROIDCONFIGURATION"))     return (void *)&IID_CONFIG;
  if (!strcmp(name, "SL_IID_RECORD"))                   return (void *)&IID_RECORD;
  debugPrintf("OpenSL: unresolved dlsym(%s)\n", name);
  return NULL;
}

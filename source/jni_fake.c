/* jni_fake.c -- minimal fake JNI environment (see jni_fake.h)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "jni_fake.h"
#include "userdata.h"
#include "fmv.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

enum {
  TAG_OBJECT = 0x4f424a31,
  TAG_STRING = 0x53545231,
  TAG_ID     = 0x4d494431,
};

typedef struct { uint32_t tag; char label[96]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; char name[96]; char sig[96]; } FakeID;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label ? label : "", sizeof(o->label) - 1);
  return o;
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

// After Burner Climax reflectively resolves a large method surface (UserValue,
// age-gate, and the whole ad/IAB SDK), so keep the pool generous -- an overflow
// would collide IDs onto slot 0 and misdispatch by name.
#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS)
    return &id_pool[0];
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// outbound method dispatch by Java method name (see abc-nx-port-facts)
//
// After Burner Climax resolves Java methods reflectively and calls them through
// the JNIEnv. The age-gate/GDPR decision is computed natively from persisted
// values read via UserValue.getValueData*(key, default); returning "already
// confirmed" for the age/consent keys makes the gate/consent flow vanish.
// Everything ad/IAB/telemetry-related is a safe no-op.
// ---------------------------------------------------------------------------

// case-insensitive substring test (newlib has no strcasestr)
static int contains_ci(const char *hay, const char *needle) {
  if (!hay || !needle) return 0;
  for (const char *h = hay; *h; h++) {
    const char *a = h, *b = needle;
    while (*a && *b && (unsigned char)(*a | 0x20) == (unsigned char)(*b | 0x20)) { a++; b++; }
    if (!*b) return 1;
  }
  return 0;
}

// a persisted key that gates the age-verify / GDPR-consent flow
static int key_is_consent(const char *k) {
  return contains_ci(k, "age") || contains_ci(k, "consent") ||
         contains_ci(k, "gdpr") || contains_ci(k, "coppa") ||
         contains_ci(k, "legal");
}

#define CONSENT_CONFIRMED 3   // value that satisfies isEnoughtAge/checkConsentGDPR

// the engine asks AfterBurnerClimax.GetDataArchiveFilePath() for its data
// archive and fopen()s the result directly (see APPLICATION::GetArchiveFilePath)
extern const char *abc_data_archive_path(void);

static void *call_object(const char *name, va_list va) {
  // data archive path (the OBB) -- fopen()'d directly by the persistent system
  if (!strcmp(name, "GetDataArchiveFilePath")) return jni_make_string(abc_data_archive_path());
  // writable / app-data dir for saves + options (else it falls back to /Home/
  // which doesn't exist -> every save/config open fails). "." == the game folder
  // (cwd), so files land next to the .nro and persist.
  if (!strcmp(name, "GetApplicationDirectory") || !strcmp(name, "getLocalPath") ||
      !strcmp(name, "getBundlePath") || !strcmp(name, "getAssertPath") ||
      !strcmp(name, "getApplicationDirectory"))  return jni_make_string(".");
  // cocos writable/save root -- must be a real writable dir (trailing slash)
  if (!strcmp(name, "getCocos2dxWritablePath")) return jni_make_string("./");
  // localization / region
  if (!strcmp(name, "getLanguageCode") || !strcmp(name, "getDeviceDisplayLanguage") ||
      !strcmp(name, "getDeviceLanguage"))       return jni_make_string("en");
  if (!strcmp(name, "getRegionCode") || !strcmp(name, "getCountryCode"))
                                                return jni_make_string("us");
  // persisted string: return the stored value, else echo the caller's default
  if (!strcmp(name, "getValueDataString")) {
    void *key = va_arg(va, void *);           // key jstring
    void *def = va_arg(va, void *);           // default jstring
    const char *k = obj_str(key);
    if (ud_has(k))
      return jni_make_string(ud_get_string(k, obj_str(def)));
    return def ? def : jni_make_string("");
  }
  // identifiers / prices / paths: harmless empty
  if (!strcmp(name, "getDeviceID"))         return jni_make_string("");
  if (!strcmp(name, "getPriceStringIAP"))   return jni_make_string("");
  if (!strcmp(name, "getApkPath"))          return jni_make_string(".");
  debugPrintf("JNI: CallObjectMethod(%s) -> null\n", name);
  return NULL;
}

static juint call_boolean(const char *name, va_list va) {
  // persisted bool: stored value wins; else consent keys report granted; else default
  if (!strcmp(name, "getValueDataBool")) {
    void *key = va_arg(va, void *);
    int def = va_arg(va, int);
    const char *k = obj_str(key);
    if (ud_has(k)) return ud_get_bool(k, def);
    return key_is_consent(k) ? 1 : (def != 0);
  }
  // layout hint (not a gate) -- console/phone layout
  if (!strcmp(name, "isTablet")) return 0;
  // intro logo done (fmv.c) -> native fires the Lua OnIntroVideoFinished
  if (!strcmp(name, "HasIntroVideoFinished")) return fmv_intro_finished();
  // org.fmod.FMOD.checkInit: FMOD aborts init with OUTPUT_INIT unless true
  if (!strcmp(name, "checkInit"))          return 1;
  if (!strcmp(name, "supportsLowLatency")) return 0;
  // everything else (ads/IAB availability, headset, etc.) -> false
  return 0;
}

static juint call_int(const char *name, va_list va) {
  // persisted int: stored value wins; else age/consent keys report "confirmed"
  // (so the gate is skipped); any other key echoes the default (first-run).
  if (!strcmp(name, "getValueDataInt")) {
    void *key = va_arg(va, void *);
    int def = va_arg(va, int);
    const char *k = obj_str(key);
    if (ud_has(k)) return (juint)ud_get_int(k, def);
    return key_is_consent(k) ? CONSENT_CONFIRMED : (juint)def;
  }
  if (!strcmp(name, "getSaveAge"))       return CONSENT_CONFIRMED;
  if (!strcmp(name, "getInternetState")) return 0; // stay offline
  if (!strcmp(name, "getRotation"))      return 0; // landscape; polled per frame -- don't log
  // org.fmod.FMOD OpenSL output config (audout is fixed 48000 Hz)
  if (!strcmp(name, "getOutputSampleRate")) return 48000;
  if (!strcmp(name, "getOutputBlockSize"))  return 512;
  debugPrintf("JNI: CallIntMethod(%s) -> 0\n", name);
  return 0;
}

// long/float persisted getters: stored value wins, else echo the default
static int64_t call_long(const char *name, va_list va) {
  if (!strcmp(name, "getValueDataLong")) {
    void *key = va_arg(va, void *);
    int64_t def = va_arg(va, int64_t);
    return ud_get_long(obj_str(key), def);
  }
  return 0;
}
static float call_float(const char *name, va_list va) {
  if (!strcmp(name, "getValueDataFloat")) {
    void *key = va_arg(va, void *);
    float def = (float)va_arg(va, double);    // float promoted to double in varargs
    return ud_get_float(obj_str(key), def);
  }
  return 0.0f;
}

// void methods: persist saveValue*(key, val); everything else is a no-op
static void call_void(const char *name, va_list va) {
  if (!strcmp(name, "saveValueInt"))   { void *k = va_arg(va, void *); ud_set_int(obj_str(k), va_arg(va, int)); return; }
  if (!strcmp(name, "saveValueBool"))  { void *k = va_arg(va, void *); ud_set_bool(obj_str(k), va_arg(va, int)); return; }
  if (!strcmp(name, "saveValueLong"))  { void *k = va_arg(va, void *); ud_set_long(obj_str(k), va_arg(va, int64_t)); return; }
  if (!strcmp(name, "saveValueFloat")) { void *k = va_arg(va, void *); ud_set_float(obj_str(k), (float)va_arg(va, double)); return; }
  if (!strcmp(name, "saveValueString")){ void *k = va_arg(va, void *); void *v = va_arg(va, void *); ud_set_string(obj_str(k), obj_str(v)); return; }
  // callBackGDPRState is a per-frame outbound notifier -- ignore silently
  if (!strcmp(name, "callBackGDPRState")) return;
  // boot logo: post a request; the render thread plays it (see fmv.c)
  if (!strcmp(name, "playIntroVideo")) { fmv_request_intro(); return; }
  debugPrintf("JNI: CallVoidMethod(%s) ignored\n", name);
}

// ---------------------------------------------------------------------------
// JNIEnv function table
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("JNI: FindClass(%s)\n", name);
  return jni_make_object(name);
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; return get_id(name, sig);
}
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return jni_make_object("class"); }
static void *j_ret_obj(void *env, void *a) { (void)env; return a; } // NewGlobalRef/NewLocalRef
static void  j_void2(void *env, void *a) { (void)env; (void)a; }

// field access: object fields -> shared non-null fake (no per-frame leak),
// primitives -> 0, writes -> no-op.
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) { (void)env; (void)cls; return get_id(name, sig); }
static void *shared_field_obj(void) { static void *o = NULL; if (!o) o = jni_make_object("field"); return o; }
static void *j_GetObjectField(void *env, void *obj, void *fid) { (void)env; (void)obj; (void)fid; return shared_field_obj(); }
static juint j_GetPrimField(void *env, void *obj, void *fid) {
  (void)env; (void)obj;
  FakeID *id = (FakeID *)fid;
  // Build.VERSION.SDK_INT: real API level, else FMOD drops OpenSL (needs >=9)
  if (id && id->tag == TAG_ID && !strcmp(id->name, "SDK_INT")) return 30;
  return 0;
}
static void  j_SetField(void *env, void *obj, void *fid, ...) { (void)env; (void)obj; (void)fid; }

static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) { (void)env; (void)obj; return call_object(id->name, va); }
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) { (void)env; (void)obj; va_list va; va_start(va, id); void *r = call_object(id->name, va); va_end(va); return r; }
static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) { (void)env; (void)obj; return call_boolean(id->name, va); }
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) { (void)env; (void)obj; va_list va; va_start(va, id); juint r = call_boolean(id->name, va); va_end(va); return r; }
static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) { (void)env; (void)obj; return call_int(id->name, va); }
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) { (void)env; (void)obj; va_list va; va_start(va, id); juint r = call_int(id->name, va); va_end(va); return r; }
static int64_t j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) { (void)env; (void)obj; return call_long(id->name, va); }
static int64_t j_CallLongMethod(void *env, void *obj, FakeID *id, ...) { (void)env; (void)obj; va_list va; va_start(va, id); int64_t r = call_long(id->name, va); va_end(va); return r; }
static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) { (void)env; (void)obj; return call_float(id->name, va); }
static float j_CallFloatMethod(void *env, void *obj, FakeID *id, ...) { (void)env; (void)obj; va_list va; va_start(va, id); float r = call_float(id->name, va); va_end(va); return r; }
static void  j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) { (void)env; (void)obj; call_void(id->name, va); }
static void  j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) { (void)env; (void)obj; va_list va; va_start(va, id); call_void(id->name, va); va_end(va); }

// local reference frames: we never track local refs (everything leaks for the
// process lifetime), so Push is a no-op success and Pop passes the result back.
static juint j_PushLocalFrame(void *env, int capacity) { (void)env; (void)capacity; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }

// UTF-16 string access. Our FakeString holds UTF-8 (ASCII in practice: paths,
// identifiers), so widen to jchar (naive latin1 -> UTF-16). Caller frees via
// ReleaseStringChars.
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *s = obj_str(jstr);
  size_t n = strlen(s);
  uint16_t *out = malloc((n + 1) * sizeof(uint16_t));
  if (!out) return NULL;
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)s[i];
  out[n] = 0;
  if (is_copy) *is_copy = 1;
  return out;
}
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr; free((void *)chars);
}

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods;
  debugPrintf("JNI: RegisterNatives(%d) ignored\n", n); return 0;
}
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void  j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) {
  // log the caller (a libacb address) so we can disassemble the call site and
  // identify exactly which JNIEnv slot the engine needs. Throttled.
  static int n = 0;
  if (n++ < 96)
    debugPrintf("JNI: unimplemented slot #%d caller=%p\n", n, __builtin_return_address(0));
  return 0;
}

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_GetEnv(void *vm, void **env, int version) { (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) { (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK; }
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  ud_load(); // load persisted UserValue store before any getValueData* call

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_ret_obj; // NewGlobalRef
  env_table[22]  = (void *)j_void2;   // DeleteGlobalRef
  env_table[23]  = (void *)j_void2;   // DeleteLocalRef
  env_table[25]  = (void *)j_ret_obj; // NewLocalRef
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  // instance Call<Type>Method: index = 34 + type*3 + variant (base/V/A)
  env_table[34]  = (void *)j_CallObjectMethod;    // CallObjectMethod
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;   // CallBooleanMethod
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;       // CallIntMethod
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;      // CallLongMethod
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;     // CallFloatMethod
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;      // CallVoidMethod
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[113] = (void *)j_GetMethodID; // GetStaticMethodID
  // static CallStatic<Type>Method: index = 114 + type*3 + variant
  env_table[114] = (void *)j_CallObjectMethod;    // CallStaticObjectMethod
  env_table[115] = (void *)j_CallObjectMethodV;
  env_table[117] = (void *)j_CallBooleanMethod;   // CallStaticBooleanMethod
  env_table[118] = (void *)j_CallBooleanMethodV;
  env_table[129] = (void *)j_CallIntMethod;       // CallStaticIntMethod
  env_table[130] = (void *)j_CallIntMethodV;
  env_table[132] = (void *)j_CallLongMethod;      // CallStaticLongMethod
  env_table[133] = (void *)j_CallLongMethodV;
  env_table[135] = (void *)j_CallFloatMethod;     // CallStaticFloatMethod
  env_table[136] = (void *)j_CallFloatMethodV;
  env_table[141] = (void *)j_CallVoidMethod;      // CallStaticVoidMethod
  env_table[142] = (void *)j_CallVoidMethodV;
  // instance fields: GetFieldID=94, Get<Type>Field 95..103, Set<Type>Field 104..112
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  for (int i = 96; i <= 103; i++) env_table[i] = (void *)j_GetPrimField;
  for (int i = 104; i <= 112; i++) env_table[i] = (void *)j_SetField;
  env_table[144] = (void *)j_GetFieldID; // GetStaticFieldID
  // static fields: GetStatic<Type>Field 145..153, SetStatic<Type>Field 154..162
  env_table[145] = (void *)j_GetObjectField;
  for (int i = 146; i <= 153; i++) env_table[i] = (void *)j_GetPrimField;
  for (int i = 154; i <= 162; i++) env_table[i] = (void *)j_SetField;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}

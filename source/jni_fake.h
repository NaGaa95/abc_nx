/* jni_fake.h -- minimal fake JNI environment for After Burner Climax
 *
 * The engine is a NativeActivity, but JNI_OnLoad still caches the JavaVM and
 * the engine makes outbound reflective calls (UserValue.getValueData*,
 * Android_AgeGate.getSaveAge, Cocos2dxHelper.getCocos2dxWritablePath,
 * getLanguageCode/getRegionCode, ads/IAB...). We emulate just enough of a
 * JavaVM/JNIEnv for those to resolve and return sane values, plus we call a few
 * exported native setters (nativeSetApkPath, ...) through this same fake env.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

void jni_init(void);

void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

#endif

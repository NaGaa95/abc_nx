/* opensl.h -- fake libOpenSLES.so (FMOD audio output) backed by libnx audout */
#ifndef __OPENSL_H__
#define __OPENSL_H__

// resolve an OpenSL ES symbol by name (slCreateEngine / SL_IID_*); NULL if absent
void *opensl_dlsym(const char *name);

#endif

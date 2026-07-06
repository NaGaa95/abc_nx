/* fmv.h -- SEGA intro logo playback (ffmpeg). See fmv.c. */
#ifndef __FMV_H__
#define __FMV_H__

void fmv_request_intro(void);   // playIntroVideo JNI: request the logo
int  fmv_intro_finished(void);  // HasIntroVideoFinished JNI: 1 once played/skipped
void fmv_render_pump(void);     // render thread (eglSwapBuffersHook): play if requested
int  fmv_audio_override(void *stream, int len_bytes); // opensl.c pump: fill logo PCM

#endif

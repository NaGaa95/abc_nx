/* fps_overlay.c -- small white FPS counter drawn with a font-atlas text shader.
 * Ported from the gtactw_nx subtitle/FPS renderer.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "fps_overlay.h"
#include "font_atlas.h"

static const char text_vshader_src[] =
  "attribute vec2 aPos;\n"
  "attribute vec2 aUV;\n"
  "uniform vec2 uOff;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  vUV = aUV;\n"
  "  gl_Position = vec4(aPos + uOff, 0.0, 1.0);\n"
  "}\n";

static const char text_fshader_src[] =
  "precision mediump float;\n"
  "uniform sampler2D texFont;\n"
  "uniform vec4 uColor;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  gl_FragColor = vec4(uColor.rgb, uColor.a * texture2D(texFont, vUV).r);\n"
  "}\n";

static struct {
  int ready;
  int uploaded;
  GLuint prog;
  GLuint tex;
  GLint loc_pos, loc_uv, loc_tex, loc_off, loc_color;
} gl;

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok)
    debugPrintf("fps: shader compile failed\n");
  return s;
}

static int gl_init(void) {
  if (gl.ready)
    return gl.prog != 0;
  gl.ready = 1;
  const GLuint vs = compile_shader(GL_VERTEX_SHADER, text_vshader_src);
  const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, text_fshader_src);
  gl.prog = glCreateProgram();
  glAttachShader(gl.prog, vs);
  glAttachShader(gl.prog, fs);
  glLinkProgram(gl.prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(gl.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    debugPrintf("fps: program link failed\n");
    glDeleteProgram(gl.prog);
    gl.prog = 0;
    return 0;
  }
  gl.loc_pos = glGetAttribLocation(gl.prog, "aPos");
  gl.loc_uv = glGetAttribLocation(gl.prog, "aUV");
  gl.loc_tex = glGetUniformLocation(gl.prog, "texFont");
  gl.loc_off = glGetUniformLocation(gl.prog, "uOff");
  gl.loc_color = glGetUniformLocation(gl.prog, "uColor");
  glGenTextures(1, &gl.tex);
  return 1;
}

static int glyph_index(unsigned int cp) {
  int lo = 0, hi = FONT_COUNT - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const unsigned int g = font_codepoints[mid];
    if (cp == g) return mid;
    if (cp < g) hi = mid - 1; else lo = mid + 1;
  }
  return -1;
}

// two triangles per glyph into verts (x,y,u,v interleaved)
static int emit_line(const char *text, int len, float x, float y,
                     float gw, float gh, GLfloat *verts) {
  int quads = 0;
  for (int j = 0; j < len; j++) {
    const int idx = glyph_index((unsigned char)text[j]);
    if (idx < 0)
      continue;
    const float u0 = (float)((idx % FONT_COLS) * FONT_CELL_W) / (float)FONT_ATLAS_W;
    const float v0 = (float)((idx / FONT_COLS) * FONT_CELL_H) / (float)FONT_ATLAS_H;
    const float u1 = u0 + (float)FONT_CELL_W / (float)FONT_ATLAS_W;
    const float v1 = v0 + (float)FONT_CELL_H / (float)FONT_ATLAS_H;
    const float gx = x + j * gw;
    const float x0 = gx * 2.0f / (float)screen_width - 1.0f;
    const float x1 = (gx + gw) * 2.0f / (float)screen_width - 1.0f;
    const float y0 = 1.0f - y * 2.0f / (float)screen_height;
    const float y1 = 1.0f - (y + gh) * 2.0f / (float)screen_height;
    const GLfloat quad[24] = {
      x0, y0, u0, v0,  x1, y0, u1, v0,  x0, y1, u0, v1,
      x1, y0, u1, v0,  x1, y1, u1, v1,  x0, y1, u0, v1,
    };
    memcpy(verts + quads * 24, quad, sizeof(quad));
    quads++;
  }
  return quads;
}

// uploads the glyph atlas on first use; binds texture unit 0, so only call
// inside a texture-state save/restore region
static void atlas_ready(void) {
  if (gl.uploaded)
    return;
  GLint prev_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, FONT_ATLAS_W, FONT_ATLAS_H, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, font_atlas);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
  gl.uploaded = 1;
}

static struct {
  u64 window_start;
  u32 frames;
  char text[8];
} fps;

void fps_overlay_render(void) {
  const u64 now = armGetSystemTick();
  const u64 freq = armGetSystemTickFreq();
  fps.frames++;
  if (!fps.window_start)
    fps.window_start = now;
  if (now - fps.window_start >= freq / 2) {
    const float rate = (float)fps.frames * (float)freq / (float)(now - fps.window_start);
    snprintf(fps.text, sizeof(fps.text), "%.0f", rate);
    fps.frames = 0;
    fps.window_start = now;
  }

  if (!fps.text[0] || !gl_init())
    return;

  // glyph size scales with the screen; aspect comes from the atlas cell
  const float gh = (float)screen_height / 30.0f;
  const float gw = gh * (float)FONT_CELL_W / (float)FONT_CELL_H;
  static GLfloat verts[8 * 24];
  const int quads = emit_line(fps.text, (int)strlen(fps.text), 10.0f, 8.0f, gw, gh, verts);
  if (!quads)
    return;

  // save every bit of GL state we touch (this runs outside the engine's draws)
  GLint prev_prog, prev_active, prev_tex0, prev_array_buf, prev_viewport[4];
  GLint bsrc_rgb, bdst_rgb, bsrc_a, bdst_a;
  const GLboolean prev_blend = glIsEnabled(GL_BLEND);
  const GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);

  atlas_ready();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glViewport(0, 0, screen_width, screen_height);
  glUseProgram(gl.prog);
  glBindTexture(GL_TEXTURE_2D, gl.tex);
  glUniform1i(gl.loc_tex, 0);
  glEnableVertexAttribArray(gl.loc_pos);
  glEnableVertexAttribArray(gl.loc_uv);
  glVertexAttribPointer(gl.loc_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
  glVertexAttribPointer(gl.loc_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);

  // drop shadow first, then the white text on top
  glUniform2f(gl.loc_off, 3.0f / (float)screen_width, -3.0f / (float)screen_height);
  glUniform4f(gl.loc_color, 0.0f, 0.0f, 0.0f, 0.9f);
  glDrawArrays(GL_TRIANGLES, 0, quads * 6);
  glUniform2f(gl.loc_off, 0.0f, 0.0f);
  glUniform4f(gl.loc_color, 1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 0, quads * 6);

  glDisableVertexAttribArray(gl.loc_pos);
  glDisableVertexAttribArray(gl.loc_uv);
  glBlendFuncSeparate(bsrc_rgb, bdst_rgb, bsrc_a, bdst_a);
  if (!prev_blend) glDisable(GL_BLEND);
  if (prev_depth) glEnable(GL_DEPTH_TEST);
  if (prev_scissor) glEnable(GL_SCISSOR_TEST);
  if (prev_cull) glEnable(GL_CULL_FACE);
  glBindTexture(GL_TEXTURE_2D, prev_tex0);
  glActiveTexture(prev_active);
  glUseProgram(prev_prog);
  glBindBuffer(GL_ARRAY_BUFFER, prev_array_buf);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

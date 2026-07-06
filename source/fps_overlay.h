/* fps_overlay.h -- small on-screen FPS counter (font-atlas text)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __FPS_OVERLAY_H__
#define __FPS_OVERLAY_H__

// Count a presented frame and draw the FPS counter in the top-left corner.
// Call at swap time, with the engine's GL context current.
void fps_overlay_render(void);

#endif

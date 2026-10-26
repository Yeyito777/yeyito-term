/* macOS st video backend: VideoToolbox, AVFoundation audio, Metal frames. */
#ifndef ST_MACOS_VIDEOPLAYER_H
#define ST_MACOS_VIDEOPLAYER_H

#include <stddef.h>
#include <stdint.h>

#include "../video.h"

/* Register the backend with graphics.c.  `view` (an NSView, may be NULL in
 * tests) drives frame ticks from its display link; `redraw` asks for a
 * frame when a shown video frame changes. */
void mac_video_init(void *device, void *view, void (*redraw)(void));
void mac_video_shutdown(void);
/* Advance every player to the frame due when the next frame reaches the
 * screen at host time `target` (CACurrentMediaTime seconds).  Returns 1 when
 * a frame to show changed.  The display link calls it; tests call it. */
int mac_video_tick(double target);
/* While drawing: make `player`'s current frame the renderer texture of
 * image `serial`.  Returns 0 if it has no frame to show yet. */
int mac_video_bind(VideoPlayer *player, uint64_t serial);
/* Whether any player still needs ticks (playing, or waiting for data). */
int mac_video_active(void);
/* One line per open video for diagnostics (state, position, counters). */
size_t mac_video_describe(char *out, size_t size);

#endif

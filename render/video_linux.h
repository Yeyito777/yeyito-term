/* Linux st video backend (docs/video.md): libavcodec decode, PulseAudio
 * output.  Every function here is called on the main thread. */
#ifndef VIDEO_LINUX_H
#define VIDEO_LINUX_H

#include <stdint.h>

#include "../video.h"

enum {
	VIDEO_MATRIX_BT601 = 0,
	VIDEO_MATRIX_BT709,
	VIDEO_MATRIX_BT2020,
};

/* The current frame of a player: planar 8-bit Y, U, V (any subsampling).
 * Valid until the next video_linux_tick() or VideoBackend call. */
typedef struct {
	uint64_t seq;		/* changes whenever the frame does */
	int width, height;	/* luma size */
	const unsigned char *plane[3];
	int linesize[3];
	int plane_width[3], plane_height[3];
	int matrix;
	int full_range;
} VideoFrameView;

const VideoBackend *video_linux_backend(void);
/* An fd that becomes readable when a decoder or the audio output needs the
 * main loop (a frame arrived after the queue ran empty, a stream drained);
 * -1 until the first video opens.  video_linux_tick() clears it. */
int video_linux_fd(void);
/* Advance every open video to now: select due frames and update playback
 * states.  Sets *changed when the frame of a video that was drawn in the
 * last frame (video_linux_drawn) changed.  Returns milliseconds until the
 * next frame or state check is due, or -1 when nothing is scheduled. */
double video_linux_tick(int *changed);
/* Display timing: the refresh period and the time of a recent vblank in
 * microseconds of CLOCK_MONOTONIC (VIDEO_TIME_NONE: unknown).  Frames are
 * then picked for the vblank a draw reaches instead of for now. */
void video_linux_set_vblank(int64_t period, int64_t vblank);
/* Bracket a presented frame: placements drawn in it mark their videos as
 * visible, so only their frame changes ask for redraws. */
void video_linux_begin_frame(void);
void video_linux_drawn(VideoPlayer *player);
/* The current frame, or 0 when none has been decoded yet. */
int video_linux_frame(VideoPlayer *player, VideoFrameView *view);
/* The current frame converted to RGBA (renderers without shaders).
 * Returns NULL when there is no frame. */
const unsigned char *video_linux_frame_rgba(VideoPlayer *player, int *width,
		int *height, uint64_t *seq);

#endif

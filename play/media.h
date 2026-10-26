/* st-play media pipeline: demux (on its own thread, so slow inputs never
 * stall the terminal loop), decode, scale and resample, and encode to
 * H.264 Annex B + raw AAC-LC, or pass H.264 through (--copy). */
#ifndef PLAY_MEDIA_H
#define PLAY_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#define MEDIA_TIME_NONE INT64_MIN
#define MEDIA_AUDIO_RATE 48000

enum {
	MEDIA_MORE,	/* did some work; call again */
	MEDIA_WAIT,	/* waiting for the demuxer (wake_fd becomes readable) */
	MEDIA_EOF,	/* everything up to the end was emitted */
	MEDIA_ERROR,
};

typedef struct {
	int64_t duration;	/* microseconds, or MEDIA_TIME_NONE */
	int has_video, has_audio;
	int width, height;	/* display size (sample aspect applied) */
	double fps;
	int seekable;
	int copyable;		/* 8-bit 4:2:0 H.264 with timestamps */
	int64_t bitrate;	/* source video bits/s estimate, 0 unknown */
	char video_desc[64], audio_desc[64];
} MediaInfo;

typedef struct Media Media;

/* stream: 0 video, 1 audio (the k=d header).  pts in microseconds. */
typedef void (*MediaEmit)(void *context, int stream, int keyframe,
		int64_t pts, const uint8_t *data, size_t length);

/* Send ffmpeg's log to a file (NULL: discard). */
void media_log_to(const char *path);
const char *media_last_log(void);

Media *media_open(const char *url, int want_audio, MediaInfo *info,
		char *error, size_t error_size);
void media_close(Media *media);
int media_wake_fd(const Media *media);
void media_drain_wake(Media *media);

/* Encoder settings, applied at the next frame: a new size, frame rate or
 * preset drains the current encoder and opens a new one (an IDR); a new
 * bitrate reconfigures it in place. */
void media_set_video(Media *media, int width, int height, double fps,
		int kbps, const char *preset);
/* Applied when the audio encoder is next opened (start, seek). */
void media_set_audio_kbps(Media *media, int kbps);
/* Pass H.264 through instead of transcoding; switches at a keyframe. */
void media_set_copy(Media *media, int copy);
int media_copying(const Media *media);
void media_encoder_size(const Media *media, int *width, int *height);
int media_encoder_kbps(const Media *media);

/* Restart at `target` (microseconds from the start): the first emitted
 * frame and audio sample are at target. */
void media_seek(Media *media, int64_t target);
int media_step(Media *media, MediaEmit emit, void *context);
const char *media_error(const Media *media);

/* x264 preset for a pixel rate (pixels per second), `faster` steps on. */
const char *media_preset_for(double pixel_rate, int faster);

#endif

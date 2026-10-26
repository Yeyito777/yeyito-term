/* st video: compressed video and audio streamed through Kitty graphics APCs
 * (a=v, docs/video.md).  This header is backend-neutral: graphics.c parses
 * the protocol with the helpers here and drives a platform VideoBackend. */
#ifndef VIDEO_H
#define VIDEO_H

#include <stddef.h>
#include <stdint.h>

#define VIDEO_MAX_OPEN          4
#define VIDEO_DIMENSION_MAX     4096
#define VIDEO_TEXT_MAX          1024U
#define VIDEO_QUEUE_MAX         (64U * 1024U * 1024U)
#define VIDEO_PACKET_HEADER     10U
#define VIDEO_AUDIO_CONFIG_MAX  64U
#define VIDEO_PREBUFFER_DEFAULT 300
#define VIDEO_PREBUFFER_MAX     10000
#define VIDEO_TIME_NONE         INT64_MIN

enum {
	VIDEO_CODEC_NONE = 0,
	VIDEO_CODEC_H264 = 1,
};

enum {
	AUDIO_CODEC_NONE = 0,
	AUDIO_CODEC_AAC = 1,
};

enum {
	VIDEO_STREAM_VIDEO = 0,
	VIDEO_STREAM_AUDIO = 1,
};

enum {
	VIDEO_STATE_BUFFERING = 0,
	VIDEO_STATE_PLAYING,
	VIDEO_STATE_PAUSED,
	VIDEO_STATE_ENDED,
};

typedef struct {
	int video_codec;
	int width;
	int height;
	int audio_codec;
	int sample_rate;
	int channels;
	unsigned char audio_config[VIDEO_AUDIO_CONFIG_MAX];
	size_t audio_config_length;
	int prebuffer_ms;
	int paused;
	double volume;
} VideoConfig;

typedef struct {
	int stream;
	int keyframe;
	int64_t pts;	/* microseconds */
	const unsigned char *data;
	size_t length;
} VideoPacket;

typedef struct {
	int state;
	int64_t position;	/* VIDEO_TIME_NONE before the first frame */
	int64_t video_end;	/* newest queued video pts, or VIDEO_TIME_NONE */
	int64_t audio_end;
	uint64_t shown;
	uint64_t dropped;
	/* Static string "CODE" or "CODE:detail" of the first error since the
	 * open or flush; NULL if none.  The backend reports decode errors here. */
	const char *error;
} VideoStatus;

/* Opaque platform player, one per open video. */
typedef struct VideoPlayer VideoPlayer;

/* Implemented by macos/video.m and render/video_linux.c, registered with
 * graphics_set_video_backend().  Every call is made on the main thread.
 * create() returns NULL and sets *error ("CODE:detail", static) on failure;
 * packet() returns 0 and sets *error for a packet it refused. */
typedef struct {
	const char *capabilities;	/* e.g. "video=h264 audio=aac max=4096x4096" */
	VideoPlayer *(*create)(const VideoConfig *config, const char **error);
	void (*destroy)(VideoPlayer *player);
	int (*packet)(VideoPlayer *player, const VideoPacket *packet,
			const char **error);
	void (*flush)(VideoPlayer *player, int64_t start);
	void (*end)(VideoPlayer *player);
	void (*pause)(VideoPlayer *player, int paused);
	void (*volume)(VideoPlayer *player, double volume);
	void (*status)(VideoPlayer *player, VideoStatus *status);
} VideoBackend;

/* Parsed `k=s` settings: each field is -1 (or < 0 volume) when absent. */
typedef struct {
	int pause;
	double volume;
} VideoSettings;

int video_parse_config(const unsigned char *text, size_t length,
		VideoConfig *config, const char **error);
int video_parse_packet(const unsigned char *data, size_t length,
		VideoPacket *packet, const char **error);
int video_parse_settings(const unsigned char *text, size_t length,
		VideoSettings *settings, const char **error);
/* Look up one integer key ("seq", "start") in a text payload. */
int video_text_int(const unsigned char *text, size_t length, const char *key,
		int64_t *value);
/* Derive an AAC-LC AudioSpecificConfig from rate and channels. */
size_t video_aac_config(int sample_rate, int channels, unsigned char *out);
const char *video_state_name(int state);
/* Format the k=q reply message ("OK seq=... state=..."); has_seq selects
 * whether seq is echoed.  Returns the length written (truncated to size). */
size_t video_format_status(char *out, size_t size, const VideoStatus *status,
		int has_seq, int64_t seq, uint64_t received);

#endif

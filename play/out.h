/* st-play output: APC framing (docs/video.md) and the userspace write queue.
 * Pure code, no I/O: st-play writes the head of the queue to the terminal
 * itself and the tests drive it directly. */
#ifndef PLAY_OUT_H
#define PLAY_OUT_H

#include <stddef.h>
#include <stdint.h>

/* Base64 characters per APC.  st accepts a whole packet in one APC, but
 * chunks keep its per-command buffers small. */
#define OUT_CHUNK 16384U

enum {
	OUT_CONTROL,	/* commands and screen text: jump ahead of media */
	OUT_PING,	/* a k=q query whose reply acknowledges the bytes before it */
	OUT_MEDIA,	/* k=d packets, limited by the in-flight window */
};

typedef struct OutMsg {
	struct OutMsg *next;
	int kind;
	int tag;	/* caller's label (st-play: status line, so it is replaced) */
	uint32_t seq;	/* OUT_PING */
	int64_t pts;	/* OUT_MEDIA, microseconds */
	int stream;
	size_t length, offset;
	unsigned char data[];
} OutMsg;

typedef struct {
	OutMsg *head, *tail;
	size_t bytes;		/* unwritten bytes */
	size_t media_bytes;	/* unwritten bytes of media messages */
	size_t media_count;
} OutQueue;

size_t base64_length(size_t length);
size_t base64_encode(const unsigned char *in, size_t length, char *out);

/* Size of the APC string(s) apc_build() writes for a payload. */
size_t apc_length(size_t controls_length, size_t payload_length, size_t chunk);
/* "ESC _ G <controls>[;base64] ESC \", split into m=1 chunks of at most
 * `chunk` base64 characters.  `prefix` (may be NULL) is base64-encoded
 * before `payload` as one stream (the 10-byte packet header).  Returns the
 * bytes written to out, which must hold apc_length(). */
size_t apc_build(char *out, const char *controls, const unsigned char *prefix,
		size_t prefix_length, const unsigned char *payload,
		size_t payload_length, size_t chunk);
/* The 10-byte k=d packet header. */
void packet_header(unsigned char out[10], int stream, int keyframe, int64_t pts);

OutMsg *out_msg_new(int kind, size_t length);
/* Media go to the tail.  Control and ping messages go after the message
 * being written (a started message is never split) and any control
 * messages already ahead of the media, so commands keep their order and
 * overtake queued packets. */
void out_push(OutQueue *queue, OutMsg *msg);
/* Removes the head once it is completely written. */
void out_advance(OutQueue *queue, size_t written);
/* Drops every unstarted media message (a seek).  Returns bytes dropped. */
size_t out_drop_media(OutQueue *queue);
/* Drops unstarted messages with `tag` (an outdated status line). */
void out_drop_tag(OutQueue *queue, int tag);
void out_clear(OutQueue *queue);

#endif

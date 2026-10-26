/* st-play output framing and queue (see out.h). */
#include "out.h"

#include <stdlib.h>
#include <string.h>

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t
base64_length(size_t length)
{
	return (length + 2) / 3 * 4;
}

size_t
base64_encode(const unsigned char *in, size_t length, char *out)
{
	char *o = out;
	size_t i;

	for (i = 0; i + 3 <= length; i += 3) {
		unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 |
		    in[i + 2];
		*o++ = b64[v >> 18];
		*o++ = b64[v >> 12 & 63];
		*o++ = b64[v >> 6 & 63];
		*o++ = b64[v & 63];
	}
	if (i < length) {
		unsigned v = (unsigned)in[i] << 16;
		if (i + 1 < length)
			v |= (unsigned)in[i + 1] << 8;
		*o++ = b64[v >> 18];
		*o++ = b64[v >> 12 & 63];
		*o++ = i + 1 < length ? b64[v >> 6 & 63] : '=';
		*o++ = '=';
	}
	return (size_t)(o - out);
}

/* ESC _ G controls [; data] ESC \ for the first chunk, "m=1;" for the
 * continuations, ",m=1"/"m=0" marking all but the last. */
size_t
apc_length(size_t controls_length, size_t payload_length, size_t chunk)
{
	size_t data = base64_length(payload_length);
	size_t chunks, length;

	if (!data)
		return 3 + controls_length + 2;
	chunk &= ~(size_t)3;
	chunks = (data + chunk - 1) / chunk;
	if (chunks == 1)
		return 3 + controls_length + 1 + data + 2;
	/* first: ESC_G ctrl ,m=1 ; ... ESC\   rest: ESC_G m=1 ; ... ESC\ */
	length = 3 + controls_length + 4 + 1 + 2;
	length += (chunks - 1) * (3 + 3 + 1 + 2);
	return length + data;
}

static unsigned char *scratch;
static size_t scratch_size;

size_t
apc_build(char *out, const char *controls, const unsigned char *prefix,
		size_t prefix_length, const unsigned char *payload,
		size_t payload_length, size_t chunk)
{
	size_t total = prefix_length + payload_length;
	size_t controls_length = strlen(controls), data, pos;
	char *o = out, *encoded;

	chunk &= ~(size_t)3;
	if (!total) {
		memcpy(o, "\033_G", 3);
		memcpy(o + 3, controls, controls_length);
		memcpy(o + 3 + controls_length, "\033\\", 2);
		return 3 + controls_length + 2;
	}
	data = base64_length(total);
	/* Encode into the tail of a scratch buffer, then frame it. */
	if (scratch_size < total + data) {
		free(scratch);
		scratch_size = (total + data) * 2;
		scratch = malloc(scratch_size);
		if (!scratch) {
			scratch_size = 0;
			return 0;
		}
	}
	if (prefix_length)
		memcpy(scratch, prefix, prefix_length);
	if (payload_length)
		memcpy(scratch + prefix_length, payload, payload_length);
	encoded = (char *)scratch + total;
	base64_encode(scratch, total, encoded);
	for (pos = 0; pos < data; pos += chunk) {
		size_t n = data - pos < chunk ? data - pos : chunk;
		int last = pos + n >= data;
		memcpy(o, "\033_G", 3);
		o += 3;
		if (pos == 0) {
			memcpy(o, controls, controls_length);
			o += controls_length;
			if (!last) {
				memcpy(o, ",m=1", 4);
				o += 4;
			}
		} else {
			memcpy(o, last ? "m=0" : "m=1", 3);
			o += 3;
		}
		*o++ = ';';
		memcpy(o, encoded + pos, n);
		o += n;
		*o++ = '\033';
		*o++ = '\\';
	}
	return (size_t)(o - out);
}

void
packet_header(unsigned char out[10], int stream, int keyframe, int64_t pts)
{
	uint64_t v = (uint64_t)pts;
	int i;

	out[0] = (unsigned char)stream;
	out[1] = keyframe ? 1 : 0;
	for (i = 0; i < 8; i++)
		out[2 + i] = (unsigned char)(v >> (56 - 8 * i));
}

OutMsg *
out_msg_new(int kind, size_t length)
{
	OutMsg *msg = calloc(1, sizeof(*msg) + length);
	if (!msg)
		return NULL;
	msg->kind = kind;
	msg->length = length;
	return msg;
}

void
out_push(OutQueue *queue, OutMsg *msg)
{
	OutMsg **link = &queue->head;

	queue->bytes += msg->length;
	if (msg->kind == OUT_MEDIA) {
		queue->media_bytes += msg->length;
		queue->media_count++;
	} else {
		/* Skip a started head and the controls queued before us. */
		if (*link && (*link)->offset)
			link = &(*link)->next;
		while (*link && (*link)->kind != OUT_MEDIA)
			link = &(*link)->next;
		msg->next = *link;
		*link = msg;
		if (!msg->next)
			queue->tail = msg;
		return;
	}
	msg->next = NULL;
	if (queue->tail)
		queue->tail->next = msg;
	else
		queue->head = msg;
	queue->tail = msg;
}

static void
unlink_msg(OutQueue *queue, OutMsg **link, OutMsg *prev)
{
	OutMsg *msg = *link;

	*link = msg->next;
	if (queue->tail == msg)
		queue->tail = prev;
	queue->bytes -= msg->length - msg->offset;
	if (msg->kind == OUT_MEDIA) {
		queue->media_bytes -= msg->length - msg->offset;
		queue->media_count--;
	}
	free(msg);
}

void
out_advance(OutQueue *queue, size_t written)
{
	OutMsg *msg = queue->head;

	if (!msg)
		return;
	msg->offset += written;
	queue->bytes -= written;
	if (msg->kind == OUT_MEDIA)
		queue->media_bytes -= written;
	if (msg->offset >= msg->length) {
		msg->offset = msg->length;
		unlink_msg(queue, &queue->head, NULL);
	}
}

static size_t
drop_matching(OutQueue *queue, int media, int tag)
{
	OutMsg **link = &queue->head, *prev = NULL;
	size_t dropped = 0;

	while (*link) {
		OutMsg *msg = *link;
		int match = !msg->offset && (media ? msg->kind == OUT_MEDIA :
		    msg->kind == OUT_CONTROL && msg->tag == tag);
		if (match) {
			dropped += msg->length;
			unlink_msg(queue, link, prev);
		} else {
			prev = msg;
			link = &msg->next;
		}
	}
	return dropped;
}

size_t
out_drop_media(OutQueue *queue)
{
	return drop_matching(queue, 1, 0);
}

void
out_drop_tag(OutQueue *queue, int tag)
{
	drop_matching(queue, 0, tag);
}

void
out_clear(OutQueue *queue)
{
	while (queue->head) {
		OutMsg *next = queue->head->next;
		free(queue->head);
		queue->head = next;
	}
	queue->tail = NULL;
	queue->bytes = queue->media_bytes = queue->media_count = 0;
}

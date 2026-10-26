/* Backend-neutral parsing for st video (docs/video.md). */
#include "video.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const unsigned char *key;
	size_t key_length;
	const unsigned char *value;
	size_t value_length;
} TextField;

static int
is_space(unsigned char c)
{
	return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

/* Iterate "key=value" fields; returns 0 at the end, -1 on a malformed one. */
static int
next_field(const unsigned char **cursor, const unsigned char *end,
		TextField *field)
{
	const unsigned char *p = *cursor, *start, *equals = NULL;

	while (p < end && is_space(*p))
		p++;
	if (p == end) {
		*cursor = p;
		return 0;
	}
	start = p;
	while (p < end && !is_space(*p)) {
		if (*p == '=' && !equals)
			equals = p;
		p++;
	}
	*cursor = p;
	if (!equals || equals == start)
		return -1;
	field->key = start;
	field->key_length = (size_t)(equals - start);
	field->value = equals + 1;
	field->value_length = (size_t)(p - equals - 1);
	return 1;
}

static int
field_is(const TextField *field, const char *key)
{
	size_t length = strlen(key);
	return field->key_length == length && !memcmp(field->key, key, length);
}

static int
value_is(const TextField *field, const char *value)
{
	size_t length = strlen(value);
	return field->value_length == length && !memcmp(field->value, value, length);
}

static int
parse_int64(const unsigned char *s, size_t length, int64_t *value)
{
	uint64_t n = 0;
	size_t i = 0;
	int negative = 0;

	if (length && s[0] == '-') {
		negative = 1;
		i = 1;
	}
	if (i == length || length - i > 19)
		return 0;
	for (; i < length; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		n = n * 10 + (uint64_t)(s[i] - '0');
	}
	if (n > (uint64_t)INT64_MAX)
		return 0;
	*value = negative ? -(int64_t)n : (int64_t)n;
	return 1;
}

static int
parse_range(const TextField *field, int64_t min, int64_t max, int *out)
{
	int64_t value;
	if (!parse_int64(field->value, field->value_length, &value) ||
	    value < min || value > max)
		return 0;
	*out = (int)value;
	return 1;
}

/* A decimal fraction in [0, 1], e.g. "0", "1", "0.25", ".5". */
static int
parse_unit(const unsigned char *s, size_t length, double *value)
{
	double result = 0, scale = 1;
	size_t i = 0;
	int digits = 0, point = 0;

	if (!length || length > 16)
		return 0;
	for (; i < length; i++) {
		if (s[i] == '.' && !point) {
			point = 1;
			continue;
		}
		if (s[i] < '0' || s[i] > '9')
			return 0;
		digits++;
		if (point) {
			scale /= 10;
			result += (s[i] - '0') * scale;
		} else {
			result = result * 10 + (s[i] - '0');
		}
	}
	if (!digits || result > 1)
		return 0;
	*value = result;
	return 1;
}

static int
hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static const int aac_rates[] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
	16000, 12000, 11025, 8000, 7350,
};

size_t
video_aac_config(int sample_rate, int channels, unsigned char *out)
{
	size_t i;
	uint64_t bits = 2;	/* audioObjectType: AAC LC */
	int width = 5;

	for (i = 0; i < sizeof(aac_rates) / sizeof(aac_rates[0]); i++)
		if (aac_rates[i] == sample_rate)
			break;
	if (i < sizeof(aac_rates) / sizeof(aac_rates[0])) {
		bits = bits << 4 | i;
		width += 4;
	} else {
		bits = (bits << 4 | 0xf) << 24 | ((uint64_t)sample_rate & 0xffffff);
		width += 28;
	}
	bits = bits << 4 | ((uint64_t)channels & 0xf);
	width += 4;
	bits <<= 3;	/* frameLengthFlag, dependsOnCoreCoder, extensionFlag */
	width += 3;
	{
		size_t bytes = (size_t)(width + 7) / 8;
		bits <<= bytes * 8 - (size_t)width;
		for (i = 0; i < bytes; i++)
			out[i] = (unsigned char)(bits >> (8 * (bytes - 1 - i)));
		return bytes;
	}
}

int
video_parse_config(const unsigned char *text, size_t length,
		VideoConfig *config, const char **error)
{
	const unsigned char *cursor = text, *end = text + length;
	TextField field;
	int status, has_config = 0;

	memset(config, 0, sizeof(*config));
	config->prebuffer_ms = VIDEO_PREBUFFER_DEFAULT;
	config->volume = 1;
	if (length > VIDEO_TEXT_MAX) {
		*error = "E2BIG:video configuration too long";
		return 0;
	}
	while ((status = next_field(&cursor, end, &field)) > 0) {
		if (field_is(&field, "video")) {
			if (!value_is(&field, "h264")) {
				*error = "ENOTSUP:unsupported video codec";
				return 0;
			}
			config->video_codec = VIDEO_CODEC_H264;
		} else if (field_is(&field, "audio")) {
			if (!value_is(&field, "aac")) {
				*error = "ENOTSUP:unsupported audio codec";
				return 0;
			}
			config->audio_codec = AUDIO_CODEC_AAC;
		} else if (field_is(&field, "width")) {
			if (!parse_range(&field, 1, VIDEO_DIMENSION_MAX, &config->width))
				goto invalid;
		} else if (field_is(&field, "height")) {
			if (!parse_range(&field, 1, VIDEO_DIMENSION_MAX, &config->height))
				goto invalid;
		} else if (field_is(&field, "rate")) {
			if (!parse_range(&field, 8000, 96000, &config->sample_rate))
				goto invalid;
		} else if (field_is(&field, "channels")) {
			if (!parse_range(&field, 1, 2, &config->channels))
				goto invalid;
		} else if (field_is(&field, "prebuffer")) {
			if (!parse_range(&field, 0, VIDEO_PREBUFFER_MAX,
			    &config->prebuffer_ms))
				goto invalid;
		} else if (field_is(&field, "pause")) {
			if (!parse_range(&field, 0, 1, &config->paused))
				goto invalid;
		} else if (field_is(&field, "volume")) {
			if (!parse_unit(field.value, field.value_length, &config->volume))
				goto invalid;
		} else if (field_is(&field, "config")) {
			size_t i;
			if (!field.value_length || field.value_length % 2 ||
			    field.value_length / 2 > VIDEO_AUDIO_CONFIG_MAX)
				goto invalid;
			for (i = 0; i < field.value_length / 2; i++) {
				int hi = hex_value(field.value[2 * i]);
				int lo = hex_value(field.value[2 * i + 1]);
				if (hi < 0 || lo < 0)
					goto invalid;
				config->audio_config[i] = (unsigned char)(hi << 4 | lo);
			}
			config->audio_config_length = field.value_length / 2;
			has_config = 1;
		}
	}
	if (status < 0)
		goto invalid;
	if (!config->video_codec && !config->audio_codec) {
		*error = "EINVAL:video needs a video or audio stream";
		return 0;
	}
	if (config->video_codec && (!config->width || !config->height)) {
		*error = "EINVAL:video needs width and height";
		return 0;
	}
	if (!config->video_codec) {
		/* An audio-only stream is still an image; give it a pixel. */
		if (!config->width) config->width = 1;
		if (!config->height) config->height = 1;
	}
	if (config->audio_codec) {
		if (!config->sample_rate || !config->channels) {
			*error = "EINVAL:audio needs rate and channels";
			return 0;
		}
		if (!has_config)
			config->audio_config_length = video_aac_config(
			    config->sample_rate, config->channels,
			    config->audio_config);
	}
	return 1;

invalid:
	*error = "EINVAL:invalid video configuration";
	return 0;
}

int
video_parse_packet(const unsigned char *data, size_t length,
		VideoPacket *packet, const char **error)
{
	uint64_t pts = 0;
	int i;

	memset(packet, 0, sizeof(*packet));
	if (length <= VIDEO_PACKET_HEADER || data[0] > VIDEO_STREAM_AUDIO ||
	    (data[1] & ~1u)) {
		*error = "EINVAL:invalid video packet";
		return 0;
	}
	for (i = 0; i < 8; i++)
		pts = pts << 8 | data[2 + i];
	packet->stream = data[0];
	packet->keyframe = data[1] & 1;
	packet->pts = (int64_t)pts;
	if (packet->pts == VIDEO_TIME_NONE) {
		*error = "EINVAL:invalid video packet";
		return 0;
	}
	packet->data = data + VIDEO_PACKET_HEADER;
	packet->length = length - VIDEO_PACKET_HEADER;
	return 1;
}

int
video_parse_settings(const unsigned char *text, size_t length,
		VideoSettings *settings, const char **error)
{
	const unsigned char *cursor = text, *end = text + length;
	TextField field;
	int status;

	settings->pause = -1;
	settings->volume = -1;
	if (length > VIDEO_TEXT_MAX) {
		*error = "E2BIG:video settings too long";
		return 0;
	}
	while ((status = next_field(&cursor, end, &field)) > 0) {
		if (field_is(&field, "pause")) {
			if (!parse_range(&field, 0, 1, &settings->pause))
				goto invalid;
		} else if (field_is(&field, "volume")) {
			if (!parse_unit(field.value, field.value_length,
			    &settings->volume))
				goto invalid;
		}
	}
	if (status < 0)
		goto invalid;
	return 1;

invalid:
	*error = "EINVAL:invalid video settings";
	return 0;
}

int
video_text_int(const unsigned char *text, size_t length, const char *key,
		int64_t *value)
{
	const unsigned char *cursor = text, *end = text + length;
	TextField field;

	if (!text || length > VIDEO_TEXT_MAX)
		return 0;
	while (next_field(&cursor, end, &field) > 0)
		if (field_is(&field, key))
			return parse_int64(field.value, field.value_length, value);
	return 0;
}

const char *
video_state_name(int state)
{
	switch (state) {
	case VIDEO_STATE_PLAYING: return "playing";
	case VIDEO_STATE_PAUSED: return "paused";
	case VIDEO_STATE_ENDED: return "ended";
	default: return "buffering";
	}
}

static int64_t
time_or_none(int64_t value)
{
	return value == VIDEO_TIME_NONE ? -1 : value;
}

size_t
video_format_status(char *out, size_t size, const VideoStatus *status,
		int has_seq, int64_t seq, uint64_t received)
{
	char seqfield[32] = "", errorfield[96] = "";
	int length;

	if (!size)
		return 0;
	if (has_seq)
		snprintf(seqfield, sizeof(seqfield), "seq=%" PRId64 " ", seq);
	if (status->error) {
		size_t code = strcspn(status->error, ":");
		if (code > 64)
			code = 64;
		snprintf(errorfield, sizeof(errorfield), " error=%.*s",
		    (int)code, status->error);
	}
	length = snprintf(out, size,
	    "OK %sstate=%s pos=%" PRId64 " vbuf=%" PRId64 " abuf=%" PRId64
	    " shown=%" PRIu64 " dropped=%" PRIu64 " rx=%" PRIu64 "%s",
	    seqfield, video_state_name(status->state),
	    time_or_none(status->position), time_or_none(status->video_end),
	    time_or_none(status->audio_end), status->shown, status->dropped,
	    received, errorfield);
	if (length < 0)
		return 0;
	return (size_t)length < size ? (size_t)length : size - 1;
}

/* st video (a=v) protocol and lifecycle tests against a mock backend. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "../graphics.h"
#include "../video.h"

static Glyph line_a[8], line_b[8];

struct VideoPlayer {
	int id;
	VideoConfig config;
	int packets;
	VideoPacket last;
	unsigned char last_data[256];
	int flushes;
	int64_t flush_start;
	int ended;
	int paused;
	double volume;
	int refuse;
};

static int created, destroyed, next_player = 1;
static struct VideoPlayer *last_player;
static int drawn;
static GraphicsPlacementView last_draw;

static VideoPlayer *
mock_create(const VideoConfig *config, const char **error)
{
	struct VideoPlayer *player;
	if (config->width == 999) {
		*error = "EIO:mock refused";
		return NULL;
	}
	player = calloc(1, sizeof(*player));
	player->id = next_player++;
	player->config = *config;
	player->paused = config->paused;
	player->volume = config->volume;
	created++;
	last_player = player;
	return player;
}

static void
mock_destroy(VideoPlayer *player)
{
	if (last_player == player)
		last_player = NULL;
	free(player);
	destroyed++;
}

static int
mock_packet(VideoPlayer *player, const VideoPacket *packet, const char **error)
{
	if (player->refuse) {
		*error = "ENOBUFS:mock queue full";
		return 0;
	}
	player->packets++;
	player->last = *packet;
	memcpy(player->last_data, packet->data,
	    packet->length < sizeof(player->last_data) ? packet->length :
	    sizeof(player->last_data));
	return 1;
}

static void
mock_flush(VideoPlayer *player, int64_t start)
{
	player->flushes++;
	player->flush_start = start;
}

static void mock_end(VideoPlayer *player) { player->ended = 1; }
static void mock_pause(VideoPlayer *player, int paused) { player->paused = paused; }
static void mock_volume(VideoPlayer *player, double volume) { player->volume = volume; }

static void
mock_status(VideoPlayer *player, VideoStatus *status)
{
	status->state = player->paused ? VIDEO_STATE_PAUSED :
	    player->ended ? VIDEO_STATE_ENDED : VIDEO_STATE_PLAYING;
	status->position = 1500000;
	status->video_end = player->packets ? player->last.pts : VIDEO_TIME_NONE;
	status->shown = 3;
	status->dropped = 1;
}

static const VideoBackend mock_backend = {
	"video=h264 audio=aac max=4096x4096",
	mock_create, mock_destroy, mock_packet, mock_flush, mock_end,
	mock_pause, mock_volume, mock_status,
};

static int
line_row(Line line, int alt)
{
	if (alt)
		return INT_MIN;
	if (line == line_a)
		return 4;
	if (line == line_b)
		return 6;
	return INT_MIN;
}

static void
capture_draw(const GraphicsPlacementView *view, void *context)
{
	(void)context;
	drawn++;
	last_draw = *view;
}

static GraphicsCommandResult
command_length(const char *sequence, size_t length, Line anchor)
{
	GraphicsCommandResult result;
	graphics_handle_apc(sequence, length, anchor, 2, 0, 10, 20, 24,
	    line_row, 1, &result);
	return result;
}

static GraphicsCommandResult
command(const char *sequence, Line anchor)
{
	return command_length(sequence, strlen(sequence), anchor);
}

static void
base64(const unsigned char *source, size_t length, char *output)
{
	static const char digits[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, used = 0;
	for (i = 0; i + 2 < length; i += 3) {
		output[used++] = digits[source[i] >> 2];
		output[used++] = digits[((source[i] & 3) << 4) | (source[i+1] >> 4)];
		output[used++] = digits[((source[i+1] & 15) << 2) | (source[i+2] >> 6)];
		output[used++] = digits[source[i+2] & 63];
	}
	if (length - i == 1) {
		output[used++] = digits[source[i] >> 2];
		output[used++] = digits[(source[i] & 3) << 4];
		output[used++] = '='; output[used++] = '=';
	} else if (length - i == 2) {
		output[used++] = digits[source[i] >> 2];
		output[used++] = digits[((source[i] & 3) << 4) | (source[i+1] >> 4)];
		output[used++] = digits[(source[i+1] & 15) << 2];
		output[used++] = '=';
	}
	output[used] = '\0';
}

/* "G<controls>;" + base64(text). */
static GraphicsCommandResult
text_command(const char *controls, const char *text)
{
	static char buffer[4096];
	int prefix = snprintf(buffer, sizeof(buffer), "G%s;", controls);
	base64((const unsigned char *)text, strlen(text), buffer + prefix);
	return command(buffer, line_a);
}

static GraphicsCommandResult
packet_bytes(const char *controls, int stream, int flags, int64_t pts,
		const char *data, size_t length)
{
	unsigned char packet[256];
	static char buffer[1024];
	int i, prefix;

	packet[0] = (unsigned char)stream;
	packet[1] = (unsigned char)flags;
	for (i = 0; i < 8; i++)
		packet[2 + i] = (unsigned char)((uint64_t)pts >> (56 - 8 * i));
	memcpy(packet + 10, data, length);
	prefix = snprintf(buffer, sizeof(buffer), "G%s;", controls);
	base64(packet, 10 + length, buffer + prefix);
	return command(buffer, line_a);
}

static GraphicsCommandResult
packet_command(const char *controls, int stream, int flags, int64_t pts,
		const char *data)
{
	return packet_bytes(controls, stream, flags, pts, data, strlen(data));
}

static void
reset_state(int backend)
{
	graphics_reset();
	graphics_set_video_backend(backend ? &mock_backend : NULL);
	created = destroyed = drawn = 0;
	last_player = NULL;
	memset(&last_draw, 0, sizeof(last_draw));
}

static GraphicsCommandResult
open_video(uint32_t id)
{
	char controls[64];
	snprintf(controls, sizeof(controls), "a=v,i=%u,k=o", id);
	return text_command(controls, "video=h264 width=640 height=360 "
	    "audio=aac rate=48000 channels=2");
}

TEST(capability_query)
{
	GraphicsCommandResult result;

	reset_state(0);
	result = command("Ga=v,i=31,k=c", line_a);
	ASSERT(result.handled);
	ASSERT(strstr(result.response, "i=31;ENOSYS:") != NULL);

	reset_state(1);
	result = command("Ga=v,i=31,k=c", line_a);
	ASSERT_STR_EQ("\033_Gi=31;OK video=h264 audio=aac max=4096x4096\033\\",
	    result.response);
	ASSERT_EQ(0, (int)graphics_image_count());
	/* q=1 still answers a query; q=2 silences it. */
	result = command("Ga=v,i=31,k=c,q=1", line_a);
	ASSERT(result.response_len > 0);
	result = command("Ga=v,i=31,k=c,q=2", line_a);
	ASSERT_EQ(0, (int)result.response_len);
	/* Without an id nothing can be answered. */
	result = command("Ga=v,k=c", line_a);
	ASSERT_EQ(0, (int)result.response_len);
	/* An unavailable renderer cannot play either. */
	graphics_handle_apc("Ga=v,i=5,k=c", 12, line_a, 0, 0, 10, 20, 24,
	    line_row, 0, &result);
	ASSERT(strstr(result.response, "ENOSYS") != NULL);
}

TEST(open_creates_a_placeable_video_image)
{
	GraphicsCommandResult result;

	reset_state(1);
	result = open_video(7);
	ASSERT_STR_EQ("\033_Gi=7;OK\033\\", result.response);
	ASSERT_EQ(1, created);
	ASSERT(last_player);
	ASSERT_EQ(VIDEO_CODEC_H264, last_player->config.video_codec);
	ASSERT_EQ(640, last_player->config.width);
	ASSERT_EQ(AUDIO_CODEC_AAC, last_player->config.audio_codec);
	ASSERT_EQ(48000, last_player->config.sample_rate);
	ASSERT_EQ(2, (int)last_player->config.audio_config_length);
	ASSERT_EQ(0x11, last_player->config.audio_config[0]);
	ASSERT_EQ(0x90, last_player->config.audio_config[1]);
	ASSERT_EQ(VIDEO_PREBUFFER_DEFAULT, last_player->config.prebuffer_ms);
	ASSERT_EQ(1, (int)graphics_image_count());
	ASSERT_EQ(1, (int)graphics_video_count());
	ASSERT_EQ(0, (int)graphics_image_bytes());

	result = command("Ga=p,i=7,p=2,c=8,C=1", line_b);
	ASSERT(result.redraw);
	ASSERT(strstr(result.response, "i=7,p=2;OK") != NULL);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24, line_row,
	    capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT(last_draw.video == (VideoPlayer *)last_player);
	ASSERT(last_draw.rgba == NULL);
	ASSERT_EQ(640, last_draw.image_width);
	ASSERT_EQ(360, last_draw.image_height);
	ASSERT_EQ(8, last_draw.columns);
	/* 8 cells of 10px wide at 16:9 is 45px: three 20px rows. */
	ASSERT_EQ(3, last_draw.rows);
	ASSERT_EQ(6, last_draw.row);
}

TEST(open_rejects_bad_configurations)
{
	GraphicsCommandResult result;

	reset_state(1);
	result = text_command("a=v,i=3,k=o", "video=vp9 width=10 height=10");
	ASSERT(strstr(result.response, "i=3;ENOTSUP:") != NULL);
	result = text_command("a=v,i=3,k=o", "video=h264");
	ASSERT(strstr(result.response, "EINVAL:") != NULL);
	result = text_command("a=v,i=3,k=o", "audio=aac rate=48000");
	ASSERT(strstr(result.response, "EINVAL:") != NULL);
	result = text_command("a=v,i=3,k=o", "video=h264 width=5000 height=10");
	ASSERT(strstr(result.response, "EINVAL:") != NULL);
	result = text_command("a=v,i=3,k=o", "width=10 height=10");
	ASSERT(strstr(result.response, "EINVAL:") != NULL);
	result = text_command("a=v,i=3,k=o", "video=h264 width=999 height=10");
	ASSERT(strstr(result.response, "EIO:mock refused") != NULL);
	ASSERT_EQ(0, (int)graphics_image_count());
	ASSERT_EQ(0, created);
	/* Audio-only is an image of one pixel. */
	result = text_command("a=v,i=4,k=o,q=1",
	    "audio=aac rate=44100 channels=1 volume=0.5 pause=1 prebuffer=900");
	ASSERT_EQ(0, (int)result.response_len);
	ASSERT(last_player);
	ASSERT_EQ(1, last_player->config.width);
	ASSERT_EQ(1, last_player->paused);
	ASSERT_EQ(900, last_player->config.prebuffer_ms);
	ASSERT(last_player->volume > 0.49 && last_player->volume < 0.51);
	/* 44.1 kHz mono AAC-LC: 00010 0100 0001 000. */
	ASSERT_EQ(0x12, last_player->config.audio_config[0]);
	ASSERT_EQ(0x08, last_player->config.audio_config[1]);
}

TEST(packets_reach_the_backend)
{
	GraphicsCommandResult result;
	char first[64], rest[64];
	unsigned char packet[13] = {0, 1, 0, 0, 0, 0, 0, 0x0f, 0x42, 0x40,
	    'a', 'b', 'c'};
	char encoded[32];

	reset_state(1);
	open_video(9);
	result = packet_bytes("a=v,i=9,k=d", VIDEO_STREAM_VIDEO, 1,
	    40000, "\x00\x00\x00\x01\x65", 5);
	ASSERT_EQ(0, (int)result.response_len);
	ASSERT(!result.redraw);
	ASSERT_EQ(1, last_player->packets);
	ASSERT_EQ(VIDEO_STREAM_VIDEO, last_player->last.stream);
	ASSERT_EQ(1, last_player->last.keyframe);
	ASSERT(last_player->last.pts == 40000);
	ASSERT_EQ(5, (int)last_player->last.length);
	ASSERT_EQ(0x65, last_player->last_data[4]);

	result = packet_command("a=v,i=9,k=d", VIDEO_STREAM_AUDIO, 0,
	    -21333, "xy");
	ASSERT_EQ(2, last_player->packets);
	ASSERT_EQ(VIDEO_STREAM_AUDIO, last_player->last.stream);
	ASSERT(last_player->last.pts == -21333);

	/* A chunked packet: pts 1000000 split across two APCs. */
	base64(packet, sizeof(packet), encoded);
	snprintf(first, sizeof(first), "Ga=v,i=9,k=d,m=1;%.8s", encoded);
	snprintf(rest, sizeof(rest), "Gm=0;%s", encoded + 8);
	result = command(first, line_a);
	ASSERT_EQ(2, last_player->packets);
	result = command(rest, line_a);
	ASSERT_EQ(3, last_player->packets);
	ASSERT(last_player->last.pts == 1000000);
	ASSERT_EQ(3, (int)last_player->last.length);
	ASSERT_EQ('c', last_player->last_data[2]);
	ASSERT_EQ(0, (int)result.response_len);
}

TEST(status_reports_progress_and_first_error)
{
	GraphicsCommandResult result;

	reset_state(1);
	open_video(9);
	packet_command("a=v,i=9,k=d", 0, 1, 2000000, "abcd");
	result = text_command("a=v,i=9,k=q", "seq=12");
	ASSERT_STR_EQ("\033_Gi=9;OK seq=12 state=playing pos=1500000 "
	    "vbuf=2000000 abuf=-1 shown=3 dropped=1 rx=14\033\\",
	    result.response);

	/* A malformed packet replies nothing and is reported once. */
	result = command("Ga=v,i=9,k=d;AAAA", line_a);
	ASSERT_EQ(0, (int)result.response_len);
	last_player->refuse = 1;
	packet_command("a=v,i=9,k=d", 0, 0, 2040000, "abcd");
	result = command("Ga=v,i=9,k=q", line_a);
	ASSERT(strstr(result.response, ";OK state=playing ") != NULL);
	ASSERT(strstr(result.response, " error=EINVAL\033\\") != NULL);
	result = command("Ga=v,i=9,k=q,q=2", line_a);
	ASSERT_EQ(0, (int)result.response_len);

	/* Flush clears it and passes the exact seek start along. */
	last_player->refuse = 0;
	result = text_command("a=v,i=9,k=f", "start=7000000");
	ASSERT_STR_EQ("\033_Gi=9;OK\033\\", result.response);
	ASSERT_EQ(1, last_player->flushes);
	ASSERT(last_player->flush_start == 7000000);
	result = command("Ga=v,i=9,k=f,q=1", line_a);
	ASSERT_EQ(0, (int)result.response_len);
	ASSERT(last_player->flush_start == VIDEO_TIME_NONE);
	result = command("Ga=v,i=9,k=q", line_a);
	ASSERT(strstr(result.response, "error=") == NULL);

	result = command("Ga=v,i=10,k=q", line_a);
	ASSERT_STR_EQ("\033_Gi=10;ENOENT:video not found\033\\",
	    result.response);
	/* Packets for an unknown video stay silent. */
	result = packet_command("a=v,i=10,k=d", 0, 1, 0, "x");
	ASSERT_EQ(0, (int)result.response_len);
}

TEST(set_and_end)
{
	GraphicsCommandResult result;

	reset_state(1);
	open_video(9);
	result = text_command("a=v,i=9,k=s,q=1", "pause=1");
	ASSERT_EQ(0, (int)result.response_len);
	ASSERT_EQ(1, last_player->paused);
	ASSERT(last_player->volume > 0.99);
	result = text_command("a=v,i=9,k=s", "volume=.25 pause=0");
	ASSERT_STR_EQ("\033_Gi=9;OK\033\\", result.response);
	ASSERT_EQ(0, last_player->paused);
	ASSERT(last_player->volume > 0.24 && last_player->volume < 0.26);
	result = text_command("a=v,i=9,k=s", "volume=2");
	ASSERT(strstr(result.response, "EINVAL") != NULL);
	result = command("Ga=v,i=9,k=e", line_a);
	ASSERT_EQ(1, last_player->ended);
	result = command("Ga=v,i=9,k=z", line_a);
	ASSERT(strstr(result.response, "EINVAL:unknown video command") != NULL);
}

TEST(reopen_replaces_video_and_placements)
{
	GraphicsCommandResult result;
	struct VideoPlayer *first;

	reset_state(1);
	open_video(5);
	first = last_player;
	command("Ga=p,i=5,c=4,C=1", line_a);
	ASSERT_EQ(1, (int)graphics_placement_count());
	result = open_video(5);
	ASSERT(result.redraw);
	ASSERT(last_player != first);
	ASSERT_EQ(1, destroyed);
	ASSERT_EQ(1, (int)graphics_video_count());
	ASSERT_EQ(0, (int)graphics_placement_count());

	/* Opening over a still image replaces it too. */
	command("Ga=T,f=32,s=1,v=1,i=6,c=1,C=1;/wAA/w==", line_a);
	ASSERT_EQ(2, (int)graphics_image_count());
	open_video(6);
	ASSERT_EQ(2, (int)graphics_image_count());
	ASSERT_EQ(2, (int)graphics_video_count());
}

TEST(delete_and_reset_close_videos)
{
	reset_state(1);
	open_video(5);
	command("Ga=p,i=5,c=4,C=1", line_a);
	/* Deleting placements keeps it playing. */
	command("Ga=d,d=i,i=5", line_a);
	ASSERT_EQ(0, (int)graphics_placement_count());
	ASSERT_EQ(1, (int)graphics_video_count());
	ASSERT_EQ(0, destroyed);
	/* An unplaced video is never evicted like an unplaced image. */
	command("Ga=d,d=I,i=5", line_a);
	ASSERT_EQ(1, destroyed);
	ASSERT_EQ(0, (int)graphics_video_count());
	ASSERT_EQ(0, (int)graphics_image_count());

	open_video(1);
	open_video(2);
	graphics_reset();
	ASSERT_EQ(3, destroyed);
	ASSERT_EQ(0, (int)graphics_video_count());
}

TEST(open_video_limit)
{
	GraphicsCommandResult result;
	uint32_t id;

	reset_state(1);
	for (id = 1; id <= VIDEO_MAX_OPEN; id++)
		open_video(id);
	result = open_video(99);
	ASSERT(strstr(result.response, "i=99;ENOSPC:") != NULL);
	ASSERT_EQ(VIDEO_MAX_OPEN, (int)graphics_video_count());
	/* Replacing one of them is not a new video. */
	result = open_video(2);
	ASSERT_STR_EQ("\033_Gi=2;OK\033\\", result.response);
	ASSERT_EQ(VIDEO_MAX_OPEN, (int)graphics_video_count());
}

TEST(config_text_parsing)
{
	VideoConfig config;
	const char *error = NULL;
	unsigned char asc[8];
	const char *text = "audio=aac\nrate=8000 channels=2 config=1390 x=y";
	int64_t value;

	ASSERT(video_parse_config((const unsigned char *)text, strlen(text),
	    &config, &error));
	ASSERT_EQ(2, (int)config.audio_config_length);
	ASSERT_EQ(0x13, config.audio_config[0]);
	ASSERT(!video_parse_config((const unsigned char *)"audio=aac rate=48000 channels=2 config=1",
	    40, &config, &error));
	ASSERT(!video_parse_config((const unsigned char *)"video=h264 =1", 13,
	    &config, &error));
	/* A rate outside the table uses the explicit 24-bit escape. */
	ASSERT_EQ(5, (int)video_aac_config(12345, 2, asc));
	ASSERT_EQ(0x17, asc[0]);
	ASSERT_EQ(0x80, asc[1]);
	ASSERT(video_text_int((const unsigned char *)"a=1 seq=-42", 11, "seq",
	    &value));
	ASSERT(value == -42);
	ASSERT(!video_text_int((const unsigned char *)"seq=4x", 6, "seq",
	    &value));
}

TEST_SUITE(video)
{
	RUN_TEST(capability_query);
	RUN_TEST(open_creates_a_placeable_video_image);
	RUN_TEST(open_rejects_bad_configurations);
	RUN_TEST(packets_reach_the_backend);
	RUN_TEST(status_reports_progress_and_first_error);
	RUN_TEST(set_and_end);
	RUN_TEST(reopen_replaces_video_and_placements);
	RUN_TEST(delete_and_reset_close_videos);
	RUN_TEST(open_video_limit);
	RUN_TEST(config_text_parsing);
	graphics_reset();
}

int
main(void)
{
	printf("video test suite\n");
	RUN_SUITE(video);
	return test_summary();
}

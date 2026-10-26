/* Linux video backend (render/video_linux.c) against real decoders, a fake
 * clock and the null audio output: no X, no sound server.  Streams are
 * encoded in-process (libx264, FFmpeg's AAC); decoding runs on the
 * backend's threads, so every wait is bounded. */
#include "../render/video_linux.c"

#include <libavutil/opt.h>

#include "test.h"

#define FPS 30
#define FRAME_US (1000000 / FPS)
/* Timestamps as the encoder produced them. */
#define PTS(i) ((int64_t)(i) * 1000000 / FPS)

static int64_t fake_now;

static int64_t
fake_clock(void)
{
	return fake_now;
}

typedef struct {
	unsigned char *data;
	size_t length;
	int64_t pts;
	int keyframe;
} Encoded;

typedef struct {
	Encoded *packets;
	int count, cap;
} Stream;

static void
stream_add(Stream *s, const AVPacket *pkt, int64_t pts)
{
	if (s->count == s->cap) {
		s->cap = s->cap ? 2 * s->cap : 64;
		s->packets = realloc(s->packets, s->cap * sizeof(*s->packets));
	}
	s->packets[s->count].data = malloc(pkt->size);
	memcpy(s->packets[s->count].data, pkt->data, pkt->size);
	s->packets[s->count].length = pkt->size;
	s->packets[s->count].pts = pts;
	s->packets[s->count].keyframe = !!(pkt->flags & AV_PKT_FLAG_KEY);
	s->count++;
}

static void
stream_free(Stream *s)
{
	int i;
	for (i = 0; i < s->count; i++)
		free(s->packets[i].data);
	free(s->packets);
	memset(s, 0, sizeof(*s));
}

/* The flat luma of frame i, so the shown frame identifies itself. */
static int
frame_luma(int i)
{
	return 32 + (i * 11) % 180;
}

static void
drain_encoder(AVCodecContext *ctx, AVPacket *pkt, Stream *out,
		int64_t scale_num, int64_t scale_den)
{
	while (avcodec_receive_packet(ctx, pkt) == 0) {
		stream_add(out, pkt, pkt->pts * scale_num / scale_den);
		av_packet_unref(pkt);
	}
}

/* H.264 Annex B with SPS/PPS on every keyframe, frames first..first+n-1. */
static int
encode_video(int width, int height, int first, int count, int gop,
		int bframes, Stream *out)
{
	const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	char params[96];
	int i;

	if (!codec || !(ctx = avcodec_alloc_context3(codec)))
		return 0;
	ctx->width = width;
	ctx->height = height;
	ctx->time_base = (AVRational){1, FPS};
	ctx->framerate = (AVRational){FPS, 1};
	ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	ctx->gop_size = gop;
	ctx->max_b_frames = bframes;
	ctx->thread_count = 1;
	av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
	snprintf(params, sizeof(params),
	    "repeat-headers=1:bframes=%d:keyint=%d:min-keyint=%d:scenecut=0",
	    bframes, gop, gop);
	av_opt_set(ctx->priv_data, "x264-params", params, 0);
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return 0;
	}
	frame = av_frame_alloc();
	pkt = av_packet_alloc();
	frame->format = AV_PIX_FMT_YUV420P;
	frame->width = width;
	frame->height = height;
	av_frame_get_buffer(frame, 0);
	for (i = first; i < first + count; i++) {
		int y;
		av_frame_make_writable(frame);
		for (y = 0; y < height; y++)
			memset(frame->data[0] + y * frame->linesize[0],
			    frame_luma(i), width);
		for (y = 0; y < height / 2; y++) {
			memset(frame->data[1] + y * frame->linesize[1], 128,
			    width / 2);
			memset(frame->data[2] + y * frame->linesize[2], 128,
			    width / 2);
		}
		frame->pts = i;
		frame->pict_type = (i - first) % gop ? AV_PICTURE_TYPE_NONE :
		    AV_PICTURE_TYPE_I;
		avcodec_send_frame(ctx, frame);
		drain_encoder(ctx, pkt, out, 1000000, FPS);
	}
	avcodec_send_frame(ctx, NULL);
	drain_encoder(ctx, pkt, out, 1000000, FPS);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&ctx);
	return out->count > 0;
}

/* Raw AAC-LC frames of a 440 Hz tone, 48 kHz stereo. */
static int
encode_audio(double seconds, Stream *out)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	int64_t n = 0, total = (int64_t)(seconds * 48000);

	if (!codec || !(ctx = avcodec_alloc_context3(codec)))
		return 0;
	ctx->sample_rate = 48000;
	ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	ctx->bit_rate = 128000;
	ctx->time_base = (AVRational){1, 48000};
	av_channel_layout_default(&ctx->ch_layout, 2);
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return 0;
	}
	frame = av_frame_alloc();
	pkt = av_packet_alloc();
	frame->format = AV_SAMPLE_FMT_FLTP;
	frame->sample_rate = 48000;
	frame->nb_samples = ctx->frame_size;
	av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
	av_frame_get_buffer(frame, 0);
	while (n < total) {
		int i;
		av_frame_make_writable(frame);
		for (i = 0; i < frame->nb_samples; i++) {
			float v = 0.25f * (float)sin(2 * M_PI * 440 * (n + i) / 48000.0);
			((float *)frame->data[0])[i] = v;
			((float *)frame->data[1])[i] = v;
		}
		frame->pts = n;
		avcodec_send_frame(ctx, frame);
		drain_encoder(ctx, pkt, out, 1000000, 48000);
		n += frame->nb_samples;
	}
	avcodec_send_frame(ctx, NULL);
	drain_encoder(ctx, pkt, out, 1000000, 48000);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&ctx);
	return out->count > 0;
}

static VideoConfig
config_for(int video, int audio, int width, int height, int prebuffer_ms)
{
	VideoConfig config;
	memset(&config, 0, sizeof(config));
	if (video) {
		config.video_codec = VIDEO_CODEC_H264;
		config.width = width;
		config.height = height;
	}
	if (audio) {
		config.audio_codec = AUDIO_CODEC_AAC;
		config.sample_rate = 48000;
		config.channels = 2;
		config.audio_config_length = video_aac_config(48000, 2,
		    config.audio_config);
	}
	config.prebuffer_ms = prebuffer_ms;
	config.volume = 1;
	return config;
}

static int
send_packets(VideoPlayer *p, const Stream *s, int stream, int from, int to)
{
	const char *error = NULL;
	int i;
	for (i = from; i < to && i < s->count; i++) {
		VideoPacket packet = {stream, s->packets[i].keyframe,
		    s->packets[i].pts, s->packets[i].data, s->packets[i].length};
		if (!vl_backend.packet(p, &packet, &error))
			return 0;
	}
	return 1;
}

static int
decoded_count(VideoPlayer *p)
{
	int n;
	pthread_mutex_lock(&p->lock);
	n = p->nframes;
	pthread_mutex_unlock(&p->lock);
	return n;
}

static uint64_t
ring_frames(VideoPlayer *p)
{
	uint64_t n;
	pthread_mutex_lock(&p->ring.lock);
	n = p->ring.pushed;
	pthread_mutex_unlock(&p->ring.lock);
	return n;
}

static void
nap(void)
{
	struct timespec ts = {0, 1000000};
	nanosleep(&ts, NULL);
}

/* Tick (without moving the fake clock) until cond holds; at most 5 s of
 * real time for the decoder threads. */
#define WAIT_FOR(cond) do { \
	int waited_, ok_ = 0; \
	for (waited_ = 0; waited_ < 5000; waited_++) { \
		if ((ok_ = !!(cond))) \
			break; \
		video_linux_tick(NULL); \
		nap(); \
	} \
	ASSERT(ok_); \
} while (0)

/* The decoder threads wait for packets or for room, with nothing in hand. */
static int
idle(VideoPlayer *p)
{
	int video, audio, ringfull;

	pthread_mutex_lock(&p->ring.lock);
	ringfull = p->has_audio &&
	    p->ring.pushed - p->ring.pulled == p->ring.capacity;
	pthread_mutex_unlock(&p->ring.lock);
	pthread_mutex_lock(&p->lock);
	video = !p->has_video || (!__atomic_load_n(&p->vbusy, __ATOMIC_RELAXED) &&
	    (p->nframes >= p->frame_cap ||
	    (!p->vq.head && (!p->eos || p->vdrained))));
	audio = !p->has_audio || (!__atomic_load_n(&p->abusy, __ATOMIC_RELAXED) &&
	    (ringfull || (!p->aq.head && (!p->eos || p->adrained))));
	pthread_mutex_unlock(&p->lock);
	return video && audio;
}

#define SETTLE(p) WAIT_FOR(idle(p))

static VideoStatus
status_of(VideoPlayer *p)
{
	VideoStatus status;
	memset(&status, 0, sizeof(status));
	vl_backend.status(p, &status);
	return status;
}

static int
current_luma(VideoPlayer *p)
{
	VideoFrameView view;
	if (!video_linux_frame(p, &view))
		return -1;
	return view.plane[0][view.linesize[0] * (view.height / 2) +
	    view.width / 2];
}

/* The shown frame's index among the first n, by its luma. */
static int
current_index(VideoPlayer *p, int n)
{
	int luma = current_luma(p), i, best = -1, diff = 1000;
	for (i = 0; i < n; i++) {
		int d = abs(frame_luma(i) - luma);
		if (d < diff) {
			diff = d;
			best = i;
		}
	}
	return luma < 0 ? -1 : best;
}

static Stream clip;	/* 64x48, 60 frames (2 s), keyframe every 15 */
static int have_x264;

TEST(video_buffers_then_plays_on_the_clock)
{
	VideoConfig config = config_for(1, 0, 64, 48, 300);
	const char *error = NULL;
	VideoPlayer *p;
	VideoStatus st;
	int64_t t0;

	if (!have_x264)
		return;
	fake_now = 1000000;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	st = status_of(p);
	ASSERT_EQ(VIDEO_STATE_BUFFERING, st.state);
	ASSERT(st.position == VIDEO_TIME_NONE);
	/* 200 ms of media: less than the prebuffer. */
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 7));
	WAIT_FOR(p->current != NULL);
	ASSERT_EQ(VIDEO_STATE_BUFFERING, status_of(p).state);
	ASSERT_EQ(0, current_index(p, 60));	/* first frame shows at once */
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 7, 30));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	t0 = fake_now;
	WAIT_FOR(decoded_count(p) >= 8);
	/* Frames follow the clock. */
	fake_now = t0 + 5 * FRAME_US + 1000;
	video_linux_tick(NULL);
	ASSERT_EQ(5, current_index(p, 60));
	st = status_of(p);
	ASSERT(st.position == 5 * FRAME_US + 1000);
	ASSERT(st.video_end == PTS(29));
	ASSERT(st.shown == 2);
	/* A jump skips frames: they count as dropped. */
	fake_now = t0 + 8 * FRAME_US + 1000;
	video_linux_tick(NULL);
	ASSERT_EQ(8, current_index(p, 60));
	st = status_of(p);
	ASSERT(st.shown == 3);
	ASSERT(st.dropped == 6);
	vl_backend.destroy(p);
}

TEST(next_frame_due_drives_the_timeout)
{
	VideoConfig config = config_for(1, 0, 64, 48, 100);
	const char *error = NULL;
	VideoPlayer *p;
	double wait;
	int changed = 0;

	if (!have_x264)
		return;
	fake_now = 5000000;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	/* Nothing open is playing: no timer. */
	ASSERT(video_linux_tick(NULL) < 0);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 20));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING &&
	    decoded_count(p) >= 2);
	fake_now += 1000;
	wait = video_linux_tick(&changed);
	/* The next frame is 32.3 ms away, rounded up to the millisecond. */
	ASSERT(wait > 32 && wait <= 33.4);
	/* Not drawn in the last frame: a change asks for no redraw. */
	fake_now += FRAME_US;
	changed = 0;
	video_linux_tick(&changed);
	ASSERT(!changed);
	video_linux_begin_frame();
	video_linux_drawn(p);
	fake_now += FRAME_US;
	video_linux_tick(&changed);
	ASSERT(changed);
	/* Paused: the clock stops and no timer is left. */
	vl_backend.pause(p, 1);
	ASSERT_EQ(VIDEO_STATE_PAUSED, status_of(p).state);
	ASSERT(video_linux_tick(NULL) < 0);
	vl_backend.destroy(p);
}

TEST(pause_freezes_the_clock)
{
	VideoConfig config = config_for(1, 0, 64, 48, 100);
	const char *error = NULL;
	VideoPlayer *p;
	int64_t pos;
	int index;

	if (!have_x264)
		return;
	fake_now = 0;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 40));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING &&
	    decoded_count(p) >= 8);
	fake_now += 3 * FRAME_US + 10;
	video_linux_tick(NULL);
	vl_backend.pause(p, 1);
	pos = status_of(p).position;
	index = current_index(p, 60);
	fake_now += 1000000;
	video_linux_tick(NULL);
	ASSERT(status_of(p).position == pos);
	ASSERT_EQ(index, current_index(p, 60));
	vl_backend.pause(p, 0);
	ASSERT_EQ(VIDEO_STATE_PLAYING, status_of(p).state);
	fake_now += 2 * FRAME_US;
	video_linux_tick(NULL);
	ASSERT(status_of(p).position == pos + 2 * FRAME_US);
	ASSERT_EQ(index + 2, current_index(p, 60));
	vl_backend.destroy(p);
}

TEST(running_dry_rebuffers_and_end_drains)
{
	VideoConfig config = config_for(1, 0, 64, 48, 200);
	const char *error = NULL;
	VideoPlayer *p;
	VideoStatus st;
	int i;

	if (!have_x264)
		return;
	fake_now = 0;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 15));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	/* The decoder holds its last frames until more data or the end; the
	 * clock catching up with the newest packet is running dry. */
	for (i = 0; i < 40 && status_of(p).state == VIDEO_STATE_PLAYING; i++) {
		fake_now += FRAME_US;
		SETTLE(p);
	}
	st = status_of(p);
	ASSERT_EQ(VIDEO_STATE_BUFFERING, st.state);
	ASSERT(st.position >= 14 * FRAME_US);
	/* The clock stays stopped while buffering. */
	fake_now += 500000;
	ASSERT(status_of(p).position == st.position);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 15, 60));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	vl_backend.end(p);
	for (i = 0; i < 100 && status_of(p).state != VIDEO_STATE_ENDED; i++) {
		fake_now += FRAME_US;
		SETTLE(p);
	}
	st = status_of(p);
	ASSERT_EQ(VIDEO_STATE_ENDED, st.state);
	ASSERT_EQ(59, current_index(p, 60));	/* the last frame stays */
	ASSERT(st.shown + st.dropped == 60);
	ASSERT(video_linux_tick(NULL) < 0);
	vl_backend.destroy(p);
}

TEST(flush_with_start_hides_earlier_frames)
{
	VideoConfig config = config_for(1, 0, 64, 48, 100);
	const char *error = NULL;
	VideoPlayer *p;
	VideoStatus st;

	if (!have_x264)
		return;
	fake_now = 0;
	config.paused = 1;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 10));
	WAIT_FOR(p->current != NULL);
	ASSERT_EQ(VIDEO_STATE_PAUSED, status_of(p).state);
	/* Seek to frame 20 of a GOP that starts at 15. */
	vl_backend.flush(p, PTS(20));
	st = status_of(p);
	ASSERT_EQ(VIDEO_STATE_PAUSED, st.state);
	ASSERT(st.video_end == VIDEO_TIME_NONE);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 15, 40));
	/* Paused, the new segment's first shown frame replaces the old one. */
	WAIT_FOR(current_index(p, 60) == 20);
	st = status_of(p);
	ASSERT(st.dropped >= 5);
	ASSERT(st.position == PTS(20));
	vl_backend.pause(p, 0);
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	fake_now += 3 * FRAME_US + 10;
	video_linux_tick(NULL);
	ASSERT_EQ(23, current_index(p, 60));
	vl_backend.destroy(p);
}

TEST(bframes_and_resolution_change)
{
	VideoConfig config = config_for(1, 0, 64, 48, 100);
	const char *error = NULL;
	VideoPlayer *p;
	VideoFrameView view;
	Stream b = {0}, big = {0};
	int i, last = -1, ordered = 1;

	if (!have_x264)
		return;
	ASSERT(encode_video(64, 48, 0, 30, 30, 2, &b));
	ASSERT(encode_video(128, 96, 30, 30, 30, 0, &big));
	fake_now = 0;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(send_packets(p, &b, VIDEO_STREAM_VIDEO, 0, b.count));
	ASSERT(send_packets(p, &big, VIDEO_STREAM_VIDEO, 0, big.count));
	vl_backend.end(p);
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	for (i = 0; i < 70; i++) {
		int index;
		SETTLE(p);
		video_linux_tick(NULL);
		index = current_index(p, 60);
		if (index < last)
			ordered = 0;
		last = index;
		fake_now += FRAME_US;
	}
	SETTLE(p);
	ASSERT(ordered);
	ASSERT_EQ(VIDEO_STATE_ENDED, status_of(p).state);
	ASSERT(status_of(p).shown == 60);
	ASSERT(video_linux_frame(p, &view));
	ASSERT_EQ(128, view.width);
	ASSERT_EQ(96, view.height);
	ASSERT_EQ(64, view.plane_width[1]);
	vl_backend.destroy(p);
	stream_free(&b);
	stream_free(&big);
}

TEST(queues_are_bounded)
{
	VideoConfig config = config_for(1, 0, 64, 48, 100);
	const char *error = NULL;
	VideoPlayer *p;
	unsigned char *junk;
	size_t accepted = 0;
	int i;

	if (!have_x264)
		return;
	fake_now = 0;
	config.paused = 1;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 60));
	/* Paused, nothing is consumed past the first frame: the decoder
	 * stops at the decoded-frame cap. */
	WAIT_FOR(decoded_count(p) == p->frame_cap);
	for (i = 0; i < 100; i++)
		nap();
	ASSERT_EQ(p->frame_cap, decoded_count(p));
	ASSERT(p->frame_cap <= VL_FRAMES_MAX);
	/* The compressed queue refuses past VIDEO_QUEUE_MAX. */
	junk = calloc(1, 1 << 20);
	for (i = 0; i < 80; i++) {
		VideoPacket packet = {VIDEO_STREAM_VIDEO, 0,
		    (int64_t)(100 + i) * FRAME_US, junk, 1 << 20};
		error = NULL;
		if (!vl_backend.packet(p, &packet, &error))
			break;
		accepted += 1 << 20;
	}
	free(junk);
	ASSERT(i < 80);
	ASSERT(error && !strncmp(error, "ENOBUFS:", 8));
	ASSERT(accepted <= VIDEO_QUEUE_MAX);
	ASSERT(accepted >= VIDEO_QUEUE_MAX - (2U << 20));
	/* A flush empties it again. */
	vl_backend.flush(p, VIDEO_TIME_NONE);
	ASSERT(p->queued == 0);
	vl_backend.destroy(p);
}

TEST(audio_clock_and_start_trim)
{
	VideoConfig config = config_for(0, 1, 1, 1, 200);
	const char *error = NULL;
	VideoPlayer *p;
	VideoStatus st;
	Stream audio = {0};
	int i;

	ASSERT(encode_audio(2.0, &audio));
	fake_now = 0;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(!p->sink.pulse);
	ASSERT(send_packets(p, &audio, VIDEO_STREAM_AUDIO, 0, 20));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	WAIT_FOR(ring_frames(p) > 48000 / 4);
	st = status_of(p);
	/* The clock is the audio heard: the null sink plays in real time. */
	fake_now += 100000;
	ASSERT(llabs(status_of(p).position - st.position - 100000) <= 100);
	/* A seek with start= drops the audio before it. */
	vl_backend.flush(p, 500000);
	ASSERT(send_packets(p, &audio, VIDEO_STREAM_AUDIO, 20, audio.count));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	SETTLE(p);
	st = status_of(p);
	ASSERT(st.position == 500000);
	fake_now += 250000;
	SETTLE(p);
	ASSERT(llabs(status_of(p).position - 750000) <= 100);
	vl_backend.end(p);
	for (i = 0; i < 100 && status_of(p).state != VIDEO_STATE_ENDED; i++) {
		fake_now += 50000;
		SETTLE(p);
	}
	st = status_of(p);
	ASSERT_EQ(VIDEO_STATE_ENDED, st.state);
	ASSERT(st.position >= 1900000 && st.position <= 2100000);
	vl_backend.destroy(p);
	stream_free(&audio);
}

TEST(audio_and_video_share_the_audio_clock)
{
	VideoConfig config = config_for(1, 1, 64, 48, 200);
	const char *error = NULL;
	VideoPlayer *p;
	Stream audio = {0};
	int i, v = 0, a = 0;

	if (!have_x264)
		return;
	ASSERT(encode_audio(2.0, &audio));
	fake_now = 0;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	/* Interleave by timestamp, as senders do. */
	while (v < clip.count || a < audio.count) {
		if (a >= audio.count || (v < clip.count &&
		    clip.packets[v].pts <= audio.packets[a].pts)) {
			ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, v, v + 1));
			v++;
		} else {
			ASSERT(send_packets(p, &audio, VIDEO_STREAM_AUDIO, a, a + 1));
			a++;
		}
	}
	vl_backend.end(p);
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	for (i = 0; i < 30; i++) {
		int64_t pos;
		int index;
		fake_now += FRAME_US;
		WAIT_FOR(decoded_count(p) > 0);
		pos = status_of(p).position;
		index = current_index(p, 60);
		/* The frame shown is the one due at the heard sample. */
		ASSERT(pos >= 0);
		ASSERT_EQ((int)(pos / FRAME_US), index);
	}
	vl_backend.destroy(p);
	stream_free(&audio);
}

TEST(frames_are_picked_for_the_next_vblank)
{
	VideoConfig config = config_for(1, 0, 64, 48, 100);
	const char *error = NULL;
	VideoPlayer *p;
	int64_t t0;
	double wait;

	if (!have_x264)
		return;
	fake_now = 7000000;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, 0, 30));
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING &&
	    decoded_count(p) >= 4);
	t0 = fake_now;	/* the clock is 0 here */
	/* 60 Hz with vblanks at 10, 26.7, 43.3, 60 ms of the clock. */
	video_linux_set_vblank(16667, t0 + 10000);
	fake_now = t0 + 5000;
	wait = video_linux_tick(NULL);
	ASSERT_EQ(0, current_index(p, 60));
	/* Frame 1 (33.3 ms) is nearest the vblank at 26.7 ms: wake 10 ms
	 * before it. */
	ASSERT(wait > 11.6 && wait < 12.7);
	/* A draw at 17 ms reaches that vblank, so frame 1 is picked before
	 * its timestamp; frame 2 (66.7 ms) waits for the one at 60 ms. */
	fake_now = t0 + 17000;
	video_linux_tick(NULL);
	ASSERT_EQ(1, current_index(p, 60));
	ASSERT(status_of(p).position == 17000);
	fake_now = t0 + 36000;	/* still reaches 43.3 ms */
	video_linux_tick(NULL);
	ASSERT_EQ(1, current_index(p, 60));
	fake_now = t0 + 51000;
	video_linux_tick(NULL);
	ASSERT_EQ(2, current_index(p, 60));
	ASSERT(status_of(p).dropped == 0);
	/* Without display timing frames wait for their timestamp. */
	video_linux_set_vblank(0, VIDEO_TIME_NONE);
	fake_now = t0 + 99000;
	video_linux_tick(NULL);
	ASSERT_EQ(2, current_index(p, 60));
	fake_now = t0 + 100100;
	video_linux_tick(NULL);
	ASSERT_EQ(3, current_index(p, 60));
	vl_backend.destroy(p);
}

TEST(audio_clock_steps_are_slewed)
{
	VideoPlayer p;
	int64_t pos, last;
	int i;

	memset(&p, 0, sizeof(p));
	p.running = 1;
	ASSERT(vl_slew(&p, 1000000, 0) == 1000000);
	/* The latency estimate updates: the audio clock steps 20 ms ahead.
	 * The clock speeds up a little instead of jumping a frame. */
	pos = vl_slew(&p, 1000000 + 16667 + 20000, 16667);
	ASSERT(pos > 1016667 && pos < 1016667 + 1000);
	last = pos;
	for (i = 2; i <= 120; i++) {
		pos = vl_slew(&p, 1020000 + i * 16667, i * 16667);
		ASSERT(pos > last && pos - last < 16667 + 1000);
		last = pos;
	}
	/* After two seconds it has caught up. */
	ASSERT(llabs(pos - (1020000 + 120 * 16667)) < 3000);
	/* A seek-sized difference jumps. */
	ASSERT(vl_slew(&p, 5000000, 121 * 16667) == 5000000);
	/* Stopped, it restarts from the audio clock. */
	p.running = 0;
	ASSERT(vl_slew(&p, 6000000, 122 * 16667) == 6000000);
}

TEST(late_audio_holds_video_on_the_host_clock)
{
	VideoConfig config = config_for(1, 1, 64, 48, 200);
	const char *error = NULL;
	VideoPlayer *p;
	Stream audio = {0};
	int64_t t0, astart;
	int i, v = 0, a = 0;

	if (!have_x264)
		return;
	video_linux_set_vblank(0, VIDEO_TIME_NONE);
	ASSERT(encode_audio(1.5, &audio));
	/* The sound starts half a second after the picture. */
	for (i = 0; i < audio.count; i++)
		audio.packets[i].pts += 500000;
	astart = audio.packets[0].pts;
	fake_now = 0;
	p = vl_backend.create(&config, &error);
	ASSERT_NOT_NULL(p);
	while (v < clip.count || a < audio.count) {
		if (a >= audio.count || (v < clip.count &&
		    clip.packets[v].pts <= audio.packets[a].pts)) {
			ASSERT(send_packets(p, &clip, VIDEO_STREAM_VIDEO, v, v + 1));
			v++;
		} else {
			ASSERT(send_packets(p, &audio, VIDEO_STREAM_AUDIO, a, a + 1));
			a++;
		}
	}
	WAIT_FOR(status_of(p).state == VIDEO_STATE_PLAYING);
	SETTLE(p);
	t0 = fake_now;
	/* Before the sound, the first frames play on the host clock. */
	for (i = 1; i <= 9; i++) {
		fake_now = t0 + i * FRAME_US + 1000;
		SETTLE(p);
		video_linux_tick(NULL);
		ASSERT_EQ(i, current_index(p, 60));
	}
	ASSERT(status_of(p).dropped == 0);
	/* Nothing was played yet. */
	ASSERT(p->audio_wait && !p->sink.running);
	/* The sound starts when the host clock reaches it ... */
	fake_now = t0 + astart + 1;
	video_linux_tick(NULL);
	ASSERT(!p->audio_wait);
	/* ... and the clock follows it from there. */
	fake_now += 100000;
	SETTLE(p);
	ASSERT(llabs(status_of(p).position - (astart + 100000)) <= 1000);
	ASSERT(ring_frames(p) > 0);
	fake_now += 100000;
	SETTLE(p);
	video_linux_tick(NULL);
	ASSERT_EQ((int)((astart + 200000) / FRAME_US), current_index(p, 60));
	vl_backend.destroy(p);
	stream_free(&audio);
}

TEST(packets_for_missing_streams_are_refused)
{
	VideoConfig config = config_for(0, 1, 1, 1, 100);
	const char *error = NULL;
	unsigned char byte = 0;
	VideoPacket packet = {VIDEO_STREAM_VIDEO, 1, 0, &byte, 1};
	VideoPlayer *p = vl_backend.create(&config, &error);

	ASSERT_NOT_NULL(p);
	ASSERT(!vl_backend.packet(p, &packet, &error));
	ASSERT(!strncmp(error, "EINVAL:", 7));
	vl_backend.destroy(p);
}

TEST_SUITE(video_linux)
{
	vl_clock = fake_clock;
	vl_null_audio = 1;
	av_log_set_level(AV_LOG_ERROR);
	have_x264 = encode_video(64, 48, 0, 60, 15, 0, &clip);
	if (!have_x264)
		printf("  (libx264 unavailable: decode tests skipped)\n");
	RUN_TEST(video_buffers_then_plays_on_the_clock);
	RUN_TEST(next_frame_due_drives_the_timeout);
	RUN_TEST(pause_freezes_the_clock);
	RUN_TEST(running_dry_rebuffers_and_end_drains);
	RUN_TEST(flush_with_start_hides_earlier_frames);
	RUN_TEST(bframes_and_resolution_change);
	RUN_TEST(queues_are_bounded);
	RUN_TEST(audio_clock_and_start_trim);
	RUN_TEST(audio_and_video_share_the_audio_clock);
	RUN_TEST(frames_are_picked_for_the_next_vblank);
	RUN_TEST(audio_clock_steps_are_slewed);
	RUN_TEST(late_audio_holds_video_on_the_host_clock);
	RUN_TEST(packets_for_missing_streams_are_refused);
	stream_free(&clip);
}

int
main(void)
{
	/* A wedged decoder thread must fail the run, not hang it. */
	alarm(120);
	printf("video_linux test suite\n");
	RUN_SUITE(video_linux);
	return test_summary();
}

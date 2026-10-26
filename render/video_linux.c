/* Linux st video backend (docs/video.md).
 *
 * Each player has a video and an audio decoder thread fed from bounded
 * compressed queues.  Decoded frames (planar YUV AVFrames, display order)
 * wait in a small queue that backpressures the video decoder; the main
 * thread picks the frame due at each tick and the renderer uploads its
 * planes.  Decoded audio goes into a ring that the PulseAudio stream pulls
 * from; the clock is the presentation time of the sample being heard
 * (written samples minus the stream's latency) while there is audio, and
 * the monotonic clock otherwise.
 *
 * Locks: player->lock guards the compressed queues, the decoded frames and
 * the decoder threads' results; ring.lock guards the audio ring.  Neither
 * is held while taking the other's... except player->lock -> ring.lock in
 * flush.  The PulseAudio mainloop lock is taken before ring.lock, never
 * while holding player->lock. */
#include "video_linux.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <pulse/pulseaudio.h>

#define VL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define VL_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Decoded frames queued ahead of the clock: 4..16, about 64 MiB at most. */
#define VL_FRAMES_MAX       16
#define VL_FRAMES_MIN       4
#define VL_FRAMES_BYTES     (64U * 1024U * 1024U)
#define VL_AUDIO_SECONDS    2
#define VL_MARKERS          1024
#define VL_PULSE_TLENGTH_MS 100
/* While playing, state changes (a stream running dry, the end) are checked
 * at least this often even when no frame is due. */
#define VL_CHECK_US         100000

typedef struct QPacket {
	struct QPacket *next;
	int64_t pts;
	int keyframe;
	size_t length;
	unsigned char data[];
} QPacket;

typedef struct {
	QPacket *head, *tail;
} PacketQueue;

typedef struct {
	uint64_t start;		/* ring frame index of its first sample */
	int64_t pts;
} AudioMarker;

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	float *samples;
	size_t capacity;	/* frames */
	int channels;
	int rate;
	uint64_t pushed, pulled, played;
	AudioMarker markers[VL_MARKERS];
	uint64_t mhead, mtail;
	uint64_t generation;
	int quit;
	float volume;
} AudioRing;

typedef struct {
	int pulse;
	pa_stream *stream;
	int running;		/* wanted uncorked (read by the pulse thread) */
	int failed;		/* set by the pulse thread */
	int64_t last;		/* null sink: clock at the last consumption */
	double carry;
} AudioSink;

struct VideoPlayer {
	VideoPlayer *next;
	VideoConfig config;
	int has_video, has_audio;
	int64_t prebuffer;
	int64_t audio_frame_us;

	/* Shared with the decoder threads under lock. */
	pthread_mutex_t lock;
	pthread_cond_t vcond, acond;
	int quit;
	uint64_t generation;
	PacketQueue vq, aq;
	size_t queued;
	int eos, vdrained, adrained;
	int64_t start;
	AVFrame *frames[VL_FRAMES_MAX];
	int nframes, frame_cap;
	uint64_t decode_dropped;
	const char *decode_error;
	AVCodecContext *vctx, *actx;
	pthread_t vthread, athread;
	int vthread_started, athread_started;
	/* Decoder threads with work in hand (not waiting for packets or
	 * room); read by tests to wait for them to settle. */
	int vbusy, abusy;

	AudioRing ring;
	AudioSink sink;

	/* Main thread only. */
	int state;
	int paused;
	int running;
	int clock_known;
	int audio_clock;
	int64_t base_pts, base_time;
	int64_t video_first, video_end, audio_first, audio_end;
	AVFrame *current;
	int stale;
	uint64_t seq;
	int changed;
	uint64_t drawn_epoch;
	uint64_t shown, dropped;
	const char *error;
	struct SwsContext *rgba_sws;
	unsigned char *rgba;
	size_t rgba_size;
	uint64_t rgba_seq;
};

static int64_t
vl_monotonic(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Test hooks: the clock and a null audio output that never touches
 * PulseAudio and consumes samples at the clock's rate. */
static int64_t (*vl_clock)(void) = vl_monotonic;
static int vl_null_audio;

static VideoPlayer *vl_players;
static int vl_fd = -1;
static uint64_t vl_epoch = 1;

static struct {
	pa_threaded_mainloop *ml;
	pa_context *ctx;
	int failed;
} vl_pa;

static void
vl_wake(void)
{
	uint64_t one = 1;
	ssize_t r;
	if (vl_fd >= 0)
		r = write(vl_fd, &one, sizeof(one));
	(void)r;
}

static void
vl_set_decode_error(VideoPlayer *p, uint64_t gen, const char *error)
{
	pthread_mutex_lock(&p->lock);
	if (p->generation == gen && !p->decode_error)
		p->decode_error = error;
	pthread_mutex_unlock(&p->lock);
}

static void
vl_queue_push(PacketQueue *q, QPacket *packet)
{
	packet->next = NULL;
	if (q->tail)
		q->tail->next = packet;
	else
		q->head = packet;
	q->tail = packet;
}

static QPacket *
vl_queue_pop(PacketQueue *q)
{
	QPacket *packet = q->head;
	if (packet) {
		q->head = packet->next;
		if (!q->head)
			q->tail = NULL;
	}
	return packet;
}

static void
vl_queue_clear(PacketQueue *q)
{
	QPacket *packet;
	while ((packet = vl_queue_pop(q)))
		free(packet);
}

/* ---- Audio ring ---- */

/* Pull up to frames frames (scaled by the volume) into out, or discard them
 * when out is NULL.  Any thread. */
static size_t
vl_ring_pull(AudioRing *r, float *out, size_t frames)
{
	size_t n, done = 0;

	pthread_mutex_lock(&r->lock);
	n = VL_MIN(frames, (size_t)(r->pushed - r->pulled));
	while (done < n) {
		size_t at = (size_t)((r->pulled + done) % r->capacity);
		size_t chunk = VL_MIN(n - done, r->capacity - at), i;
		if (out) {
			const float *src = r->samples + at * r->channels;
			float *dst = out + done * r->channels;
			for (i = 0; i < chunk * (size_t)r->channels; i++)
				dst[i] = src[i] * r->volume;
		}
		done += chunk;
	}
	r->pulled += n;
	/* Markers far behind what was handed out are certainly played. */
	while (r->mtail + 1 < r->mhead &&
	    r->markers[(r->mtail + 1) % VL_MARKERS].start + (uint64_t)r->rate *
	    VL_AUDIO_SECONDS <= r->pulled)
		r->mtail++;
	if (n)
		pthread_cond_broadcast(&r->cond);
	pthread_mutex_unlock(&r->lock);
	return n;
}

/* Audio thread: append frames of interleaved PCM starting at pts, waiting
 * for room.  Returns 0 when a flush or close discarded them. */
static int
vl_ring_push(AudioRing *r, const float *pcm, size_t frames, int64_t pts,
		uint64_t gen, int *busy)
{
	pthread_mutex_lock(&r->lock);
	while (frames) {
		size_t space, n, done = 0;
		while (!r->quit && r->generation == gen &&
		    ((space = r->capacity - (size_t)(r->pushed - r->pulled)) == 0 ||
		    r->mhead - r->mtail >= VL_MARKERS)) {
			__atomic_store_n(busy, 0, __ATOMIC_RELAXED);
			pthread_cond_wait(&r->cond, &r->lock);
		}
		__atomic_store_n(busy, 1, __ATOMIC_RELAXED);
		if (r->quit || r->generation != gen) {
			pthread_mutex_unlock(&r->lock);
			return 0;
		}
		n = VL_MIN(frames, space);
		r->markers[r->mhead % VL_MARKERS] = (AudioMarker){r->pushed, pts};
		r->mhead++;
		while (done < n) {
			size_t at = (size_t)((r->pushed + done) % r->capacity);
			size_t chunk = VL_MIN(n - done, r->capacity - at);
			memcpy(r->samples + at * r->channels,
			    pcm + done * r->channels,
			    chunk * r->channels * sizeof(float));
			done += chunk;
		}
		r->pushed += n;
		frames -= n;
		pcm += n * r->channels;
		pts += (int64_t)n * 1000000 / r->rate;
	}
	pthread_mutex_unlock(&r->lock);
	return 1;
}

/* ---- PulseAudio ---- */

static void
vl_pa_signal(pa_context *c, void *userdata)
{
	(void)c;
	(void)userdata;
	pa_threaded_mainloop_signal(vl_pa.ml, 0);
}

static void
vl_pa_timeout(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv,
		void *userdata)
{
	(void)api;
	(void)e;
	(void)tv;
	*(int *)userdata = 1;
	pa_threaded_mainloop_signal(vl_pa.ml, 0);
}

/* Connect the shared context (once, or again after the server went away).
 * Waits at most two seconds.  Returns 1 when ready. */
static int
vl_pulse_start(void)
{
	pa_context_state_t state;
	pa_time_event *timer;
	int timedout = 0, ok;

	if (vl_pa.failed)
		return 0;
	if (!vl_pa.ml) {
		if (!(vl_pa.ml = pa_threaded_mainloop_new()) ||
		    pa_threaded_mainloop_start(vl_pa.ml) < 0) {
			if (vl_pa.ml)
				pa_threaded_mainloop_free(vl_pa.ml);
			vl_pa.ml = NULL;
			vl_pa.failed = 1;
			return 0;
		}
	}
	pa_threaded_mainloop_lock(vl_pa.ml);
	if (vl_pa.ctx && pa_context_get_state(vl_pa.ctx) == PA_CONTEXT_READY) {
		pa_threaded_mainloop_unlock(vl_pa.ml);
		return 1;
	}
	if (vl_pa.ctx) {
		pa_context_set_state_callback(vl_pa.ctx, NULL, NULL);
		pa_context_disconnect(vl_pa.ctx);
		pa_context_unref(vl_pa.ctx);
	}
	vl_pa.ctx = pa_context_new(pa_threaded_mainloop_get_api(vl_pa.ml), "st");
	if (!vl_pa.ctx) {
		pa_threaded_mainloop_unlock(vl_pa.ml);
		return 0;
	}
	pa_context_set_state_callback(vl_pa.ctx, vl_pa_signal, NULL);
	if (pa_context_connect(vl_pa.ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
		pa_threaded_mainloop_unlock(vl_pa.ml);
		return 0;
	}
	timer = pa_context_rttime_new(vl_pa.ctx, pa_rtclock_now() +
	    2 * PA_USEC_PER_SEC, vl_pa_timeout, &timedout);
	for (;;) {
		state = pa_context_get_state(vl_pa.ctx);
		if (state == PA_CONTEXT_READY || !PA_CONTEXT_IS_GOOD(state) ||
		    timedout)
			break;
		pa_threaded_mainloop_wait(vl_pa.ml);
	}
	if (timer)
		pa_threaded_mainloop_get_api(vl_pa.ml)->time_free(timer);
	ok = state == PA_CONTEXT_READY;
	pa_threaded_mainloop_unlock(vl_pa.ml);
	return ok;
}

/* Hand the stream what it can take from the ring (mainloop locked). */
static void
vl_pulse_pump(VideoPlayer *p)
{
	pa_stream *s = p->sink.stream;
	size_t writable, framesize = sizeof(float) * p->ring.channels;

	if (!s || pa_stream_get_state(s) != PA_STREAM_READY)
		return;
	writable = pa_stream_writable_size(s);
	if (writable == (size_t)-1)
		return;
	while (writable >= framesize) {
		void *buffer = NULL;
		size_t bytes, n, available;

		pthread_mutex_lock(&p->ring.lock);
		available = (size_t)(p->ring.pushed - p->ring.pulled);
		pthread_mutex_unlock(&p->ring.lock);
		if (!available)
			break;
		bytes = VL_MIN(writable, available * framesize);
		if (pa_stream_begin_write(s, &buffer, &bytes) < 0 || !buffer)
			break;
		n = vl_ring_pull(&p->ring, buffer,
		    VL_MIN(bytes / framesize, available));
		if (!n) {
			pa_stream_cancel_write(s);
			break;
		}
		pa_stream_write(s, buffer, n * framesize, NULL, 0, PA_SEEK_RELATIVE);
		writable -= n * framesize;
	}
}

static void
vl_pulse_write(pa_stream *s, size_t bytes, void *userdata)
{
	(void)s;
	(void)bytes;
	vl_pulse_pump(userdata);
}

static void
vl_pulse_state(pa_stream *s, void *userdata)
{
	VideoPlayer *p = userdata;
	pa_stream_state_t state = pa_stream_get_state(s);

	if (state == PA_STREAM_READY) {
		if (__atomic_load_n(&p->sink.running, __ATOMIC_RELAXED))
			pa_operation_unref(pa_stream_cork(s, 0, NULL, NULL));
		vl_pulse_pump(p);
	} else if (!PA_STREAM_IS_GOOD(state)) {
		__atomic_store_n(&p->sink.failed, 1, __ATOMIC_RELAXED);
		vl_wake();
	}
}

static int
vl_pulse_open(VideoPlayer *p)
{
	pa_sample_spec spec;
	pa_buffer_attr attr;
	pa_stream *s;

	if (!vl_pulse_start())
		return 0;
	spec.format = PA_SAMPLE_FLOAT32NE;
	spec.rate = (uint32_t)p->ring.rate;
	spec.channels = (uint8_t)p->ring.channels;
	attr.maxlength = (uint32_t)-1;
	attr.tlength = (uint32_t)pa_usec_to_bytes(VL_PULSE_TLENGTH_MS * 1000,
	    &spec);
	/* Playback starts and stops by corking, never by waiting for data. */
	attr.prebuf = 0;
	attr.minreq = (uint32_t)-1;
	attr.fragsize = (uint32_t)-1;
	pa_threaded_mainloop_lock(vl_pa.ml);
	s = pa_stream_new(vl_pa.ctx, "video", &spec, NULL);
	if (!s) {
		pa_threaded_mainloop_unlock(vl_pa.ml);
		return 0;
	}
	pa_stream_set_state_callback(s, vl_pulse_state, p);
	pa_stream_set_write_callback(s, vl_pulse_write, p);
	if (pa_stream_connect_playback(s, NULL, &attr, PA_STREAM_START_CORKED |
	    PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
	    PA_STREAM_ADJUST_LATENCY, NULL, NULL) < 0) {
		pa_stream_set_state_callback(s, NULL, NULL);
		pa_stream_set_write_callback(s, NULL, NULL);
		pa_stream_unref(s);
		pa_threaded_mainloop_unlock(vl_pa.ml);
		return 0;
	}
	p->sink.stream = s;
	p->sink.pulse = 1;
	pa_threaded_mainloop_unlock(vl_pa.ml);
	return 1;
}

static void
vl_pulse_close(VideoPlayer *p)
{
	pa_stream *s;

	if (!p->sink.pulse)
		return;
	pa_threaded_mainloop_lock(vl_pa.ml);
	s = p->sink.stream;
	p->sink.stream = NULL;
	p->sink.pulse = 0;
	if (s) {
		pa_stream_set_state_callback(s, NULL, NULL);
		pa_stream_set_write_callback(s, NULL, NULL);
		pa_stream_disconnect(s);
		pa_stream_unref(s);
	}
	pa_threaded_mainloop_unlock(vl_pa.ml);
}

/* ---- Audio output (PulseAudio or the null sink) ---- */

static void
vl_sink_run(VideoPlayer *p, int running, int64_t now)
{
	__atomic_store_n(&p->sink.running, running, __ATOMIC_RELAXED);
	if (!p->sink.pulse) {
		p->sink.last = now;
		p->sink.carry = 0;
		return;
	}
	pa_threaded_mainloop_lock(vl_pa.ml);
	if (p->sink.stream &&
	    pa_stream_get_state(p->sink.stream) == PA_STREAM_READY) {
		pa_operation_unref(pa_stream_cork(p->sink.stream, !running,
		    NULL, NULL));
		if (running)
			vl_pulse_pump(p);
	}
	pa_threaded_mainloop_unlock(vl_pa.ml);
}

/* Drop what the output buffered; the ring was emptied before. */
static void
vl_sink_flush(VideoPlayer *p)
{
	pa_operation *op;

	if (!p->sink.pulse)
		return;
	pa_threaded_mainloop_lock(vl_pa.ml);
	if (p->sink.stream &&
	    pa_stream_get_state(p->sink.stream) == PA_STREAM_READY) {
		if ((op = pa_stream_flush(p->sink.stream, NULL, NULL)))
			pa_operation_unref(op);
		if ((op = pa_stream_update_timing_info(p->sink.stream, NULL, NULL)))
			pa_operation_unref(op);
	}
	pa_threaded_mainloop_unlock(vl_pa.ml);
}

/* Audio thread: new samples are in the ring. */
static void
vl_sink_kick(VideoPlayer *p)
{
	if (!__atomic_load_n(&p->sink.pulse, __ATOMIC_RELAXED))
		return;
	pa_threaded_mainloop_lock(vl_pa.ml);
	vl_pulse_pump(p);
	pa_threaded_mainloop_unlock(vl_pa.ml);
}

/* Frames handed to the output that are not heard yet, -1 if unknown.  The
 * null sink consumes at the clock's rate here. */
static int64_t
vl_sink_pending(VideoPlayer *p, int64_t now)
{
	int64_t pending = -1;
	pa_usec_t usec;
	int negative = 0;

	if (!p->sink.pulse) {
		if (p->sink.running && now > p->sink.last) {
			double frames = (double)(now - p->sink.last) *
			    p->ring.rate / 1e6 + p->sink.carry;
			size_t n = (size_t)frames;
			p->sink.carry = frames - (double)n;
			vl_ring_pull(&p->ring, NULL, n);
		}
		p->sink.last = now;
		return 0;
	}
	pa_threaded_mainloop_lock(vl_pa.ml);
	if (p->sink.stream &&
	    pa_stream_get_state(p->sink.stream) == PA_STREAM_READY &&
	    pa_stream_get_latency(p->sink.stream, &usec, &negative) == 0)
		pending = negative ? 0 : (int64_t)(usec * p->ring.rate / 1000000);
	pa_threaded_mainloop_unlock(vl_pa.ml);
	return pending;
}

/* The output failed (server gone): keep the timeline with the null sink. */
static void
vl_sink_fallback(VideoPlayer *p, int64_t now)
{
	vl_pulse_close(p);
	p->sink.failed = 0;
	p->sink.last = now;
	p->sink.carry = 0;
	if (!p->error)
		p->error = "EIO:audio output failed";
}

/* ---- Decoder threads ---- */

static int
vl_planar_yuv(int format)
{
	switch (format) {
	case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
	case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P:
	case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P:
		return 1;
	default:
		return 0;
	}
}

/* Queue a decoded frame (taking it), waiting while the queue is full.
 * Returns 0 when a flush or close discarded it. */
static int
vl_push_frame(VideoPlayer *p, AVFrame *frame, uint64_t gen)
{
	int i, wake;

	pthread_mutex_lock(&p->lock);
	while (!p->quit && p->generation == gen && p->nframes >= p->frame_cap) {
		__atomic_store_n(&p->vbusy, 0, __ATOMIC_RELAXED);
		pthread_cond_wait(&p->vcond, &p->lock);
	}
	__atomic_store_n(&p->vbusy, 1, __ATOMIC_RELAXED);
	if (p->quit || p->generation != gen) {
		pthread_mutex_unlock(&p->lock);
		av_frame_free(&frame);
		return 0;
	}
	for (i = p->nframes; i > 0 && p->frames[i - 1]->pts > frame->pts; i--)
		p->frames[i] = p->frames[i - 1];
	p->frames[i] = frame;
	wake = p->nframes++ == 0;
	pthread_mutex_unlock(&p->lock);
	if (wake)
		vl_wake();
	return 1;
}

static int
vl_deliver_video(VideoPlayer *p, AVFrame *frame, uint64_t gen, int64_t start,
		struct SwsContext **sws, int64_t *last)
{
	AVFrame *out;
	int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts :
	    frame->best_effort_timestamp;

	if (pts == AV_NOPTS_VALUE)
		pts = *last + 1;
	*last = pts;
	if (start != VIDEO_TIME_NONE && pts < start) {
		/* Decoded only to reach an exact seek point. */
		pthread_mutex_lock(&p->lock);
		if (p->generation == gen)
			p->decode_dropped++;
		pthread_mutex_unlock(&p->lock);
		av_frame_unref(frame);
		return 1;
	}
	if (!(out = av_frame_alloc())) {
		av_frame_unref(frame);
		return 1;
	}
	if (vl_planar_yuv(frame->format)) {
		av_frame_move_ref(out, frame);
	} else {
		out->format = AV_PIX_FMT_YUV420P;
		out->width = frame->width;
		out->height = frame->height;
		*sws = sws_getCachedContext(*sws, frame->width, frame->height,
		    frame->format, frame->width, frame->height,
		    AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
		if (!*sws || av_frame_get_buffer(out, 0) < 0) {
			vl_set_decode_error(p, gen, "ENOTSUP:unsupported video format");
			av_frame_free(&out);
			av_frame_unref(frame);
			return 1;
		}
		av_frame_copy_props(out, frame);
		sws_scale(*sws, (const uint8_t *const *)frame->data,
		    frame->linesize, 0, frame->height, out->data, out->linesize);
		av_frame_unref(frame);
	}
	out->pts = pts;
	return vl_push_frame(p, out, gen);
}

static float
vl_sample(const AVFrame *frame, int channel, int i)
{
	int channels = frame->ch_layout.nb_channels;
	switch (frame->format) {
	case AV_SAMPLE_FMT_FLTP:
		return ((const float *)frame->extended_data[channel])[i];
	case AV_SAMPLE_FMT_FLT:
		return ((const float *)frame->data[0])[i * channels + channel];
	case AV_SAMPLE_FMT_S16P:
		return ((const int16_t *)frame->extended_data[channel])[i] / 32768.0f;
	case AV_SAMPLE_FMT_S16:
		return ((const int16_t *)frame->data[0])[i * channels + channel] /
		    32768.0f;
	case AV_SAMPLE_FMT_S32P:
		return ((const int32_t *)frame->extended_data[channel])[i] /
		    2147483648.0f;
	case AV_SAMPLE_FMT_DBLP:
		return (float)((const double *)frame->extended_data[channel])[i];
	default:
		return 0;
	}
}

static int
vl_deliver_audio(VideoPlayer *p, AVFrame *frame, uint64_t gen, int64_t start,
		float **pcm, size_t *pcm_size, int64_t *next)
{
	int rate = p->ring.rate, channels = p->ring.channels;
	int in = frame->ch_layout.nb_channels, n = frame->nb_samples;
	int skip = 0, i, c, ok;
	int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : *next;
	size_t need;

	*next = pts + (int64_t)n * 1000000 / rate;
	if (in <= 0 || n <= 0)
		return 1;
	if (start != VIDEO_TIME_NONE && pts < start) {
		if (*next <= start)
			return 1;
		skip = (int)((start - pts) * rate / 1000000);
		pts = start;
	}
	need = (size_t)(n - skip) * channels * sizeof(float);
	if (need > *pcm_size) {
		float *grown = realloc(*pcm, need);
		if (!grown)
			return 1;
		*pcm = grown;
		*pcm_size = need;
	}
	for (i = skip; i < n; i++) {
		float *out = *pcm + (size_t)(i - skip) * channels;
		if (channels == 1 && in > 1) {
			out[0] = 0.5f * (vl_sample(frame, 0, i) +
			    vl_sample(frame, 1, i));
			continue;
		}
		for (c = 0; c < channels; c++)
			out[c] = vl_sample(frame, VL_MIN(c, in - 1), i);
	}
	ok = vl_ring_push(&p->ring, *pcm, (size_t)(n - skip), pts, gen,
	    &p->abusy);
	if (ok)
		vl_sink_kick(p);
	return ok;
}

/* One decoder thread per stream: pop a packet, decode, deliver every frame
 * it yields; at end of stream drain the decoder.  A flush bumps the
 * generation, which discards in-flight output and resets the decoder. */
static void *
vl_decode_thread(VideoPlayer *p, int video)
{
	AVCodecContext *ctx = video ? p->vctx : p->actx;
	PacketQueue *queue = video ? &p->vq : &p->aq;
	pthread_cond_t *cond = video ? &p->vcond : &p->acond;
	int *drained = video ? &p->vdrained : &p->adrained;
	int *busy = video ? &p->vbusy : &p->abusy;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	struct SwsContext *sws = NULL;
	float *pcm = NULL;
	size_t pcm_size = 0;
	int64_t last = 0;
	uint64_t gen;

	pthread_mutex_lock(&p->lock);
	gen = p->generation;
	while (!p->quit && pkt && frame) {
		QPacket *q = NULL;
		int64_t start;
		int ret, drain = 0, live = 1;

		if (p->generation != gen) {
			gen = p->generation;
			pthread_mutex_unlock(&p->lock);
			avcodec_flush_buffers(ctx);
			last = 0;
			pthread_mutex_lock(&p->lock);
			continue;
		}
		if (queue->head) {
			q = vl_queue_pop(queue);
			p->queued -= q->length;
		} else if (p->eos && !*drained) {
			drain = 1;
		} else {
			__atomic_store_n(busy, 0, __ATOMIC_RELAXED);
			pthread_cond_wait(cond, &p->lock);
			continue;
		}
		__atomic_store_n(busy, 1, __ATOMIC_RELAXED);
		start = p->start;
		pthread_mutex_unlock(&p->lock);

		if (q) {
			pkt->data = q->data;
			pkt->size = (int)q->length;
			pkt->pts = q->pts;
			pkt->dts = AV_NOPTS_VALUE;
			pkt->flags = q->keyframe ? AV_PKT_FLAG_KEY : 0;
			ret = avcodec_send_packet(ctx, pkt);
		} else {
			ret = avcodec_send_packet(ctx, NULL);
		}
		if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
			vl_set_decode_error(p, gen, video ?
			    "EBADMSG:undecodable video" : "EBADMSG:undecodable audio");
		while (live && avcodec_receive_frame(ctx, frame) == 0) {
			live = video ?
			    vl_deliver_video(p, frame, gen, start, &sws, &last) :
			    vl_deliver_audio(p, frame, gen, start, &pcm, &pcm_size,
			    &last);
			av_frame_unref(frame);
		}
		free(q);
		pthread_mutex_lock(&p->lock);
		if (drain && p->generation == gen) {
			*drained = 1;
			vl_wake();
		}
	}
	pthread_mutex_unlock(&p->lock);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	sws_freeContext(sws);
	free(pcm);
	return NULL;
}

static void *
vl_video_thread(void *arg)
{
	return vl_decode_thread(arg, 1);
}

static void *
vl_audio_thread(void *arg)
{
	return vl_decode_thread(arg, 0);
}

/* ---- Playback state (main thread) ---- */

static int64_t
vl_audio_position(VideoPlayer *p, int64_t now)
{
	AudioRing *r = &p->ring;
	int64_t pending = vl_sink_pending(p, now), pts;
	uint64_t played, next;
	AudioMarker *m;

	pthread_mutex_lock(&r->lock);
	played = r->played;
	if (pending >= 0)
		played = r->pulled > (uint64_t)pending ? r->pulled -
		    (uint64_t)pending : 0;
	played = VL_MIN(VL_MAX(played, r->played), r->pulled);
	r->played = played;
	while (r->mtail + 1 < r->mhead &&
	    r->markers[(r->mtail + 1) % VL_MARKERS].start <= played)
		r->mtail++;
	if (r->mtail == r->mhead) {
		pthread_mutex_unlock(&r->lock);
		return VIDEO_TIME_NONE;
	}
	m = &r->markers[r->mtail % VL_MARKERS];
	next = r->mtail + 1 < r->mhead ?
	    r->markers[(r->mtail + 1) % VL_MARKERS].start : r->pushed;
	pts = m->pts;
	if (played > m->start)
		pts += (int64_t)(VL_MIN(played, next) - m->start) * 1000000 /
		    r->rate;
	pthread_mutex_unlock(&r->lock);
	return pts;
}

/* Everything decoded was heard (or skipped). */
static int
vl_audio_heard(VideoPlayer *p, int64_t now)
{
	AudioRing *r = &p->ring;
	int done;

	vl_audio_position(p, now);
	pthread_mutex_lock(&r->lock);
	done = r->pushed == r->pulled &&
	    r->played + (uint64_t)(r->rate / 100) >= r->pushed;
	pthread_mutex_unlock(&r->lock);
	return done;
}

static int64_t
vl_position(VideoPlayer *p, int64_t now)
{
	int64_t pts;

	if (!p->clock_known)
		return VIDEO_TIME_NONE;
	if (p->audio_clock) {
		pts = vl_audio_position(p, now);
		return pts == VIDEO_TIME_NONE ? p->base_pts : pts;
	}
	return p->running ? p->base_pts + (now - p->base_time) : p->base_pts;
}

static void
vl_set_current(VideoPlayer *p, AVFrame *frame)
{
	av_frame_free(&p->current);
	p->current = frame;
	p->stale = 0;
	p->seq++;
	p->shown++;
	p->changed = 1;
}

/* Show the newest decoded frame not after the clock; while stopped, show
 * the first frame of a new segment as soon as it is decoded. */
static void
vl_select(VideoPlayer *p, int64_t pos)
{
	AVFrame *frame = NULL;
	int i, due = -1;

	pthread_mutex_lock(&p->lock);
	if (p->running && pos != VIDEO_TIME_NONE) {
		for (i = 0; i < p->nframes && p->frames[i]->pts <= pos; i++)
			due = i;
	} else if (!p->running && (!p->current || p->stale) && p->nframes) {
		due = 0;
	}
	if (due >= 0) {
		for (i = 0; i < due; i++)
			av_frame_free(&p->frames[i]);
		p->dropped += (uint64_t)due;
		frame = p->frames[due];
		memmove(p->frames, p->frames + due + 1,
		    (size_t)(p->nframes - due - 1) * sizeof(*p->frames));
		p->nframes -= due + 1;
		pthread_cond_broadcast(&p->vcond);
	}
	pthread_mutex_unlock(&p->lock);
	if (frame)
		vl_set_current(p, frame);
}

static int64_t
vl_initial_pts(VideoPlayer *p)
{
	int64_t pts = VIDEO_TIME_NONE;

	if (p->start != VIDEO_TIME_NONE)
		return p->start;
	if (p->has_video) {
		if (p->current && !p->stale)
			pts = p->current->pts;
		pthread_mutex_lock(&p->lock);
		if (pts == VIDEO_TIME_NONE && p->nframes)
			pts = p->frames[0]->pts;
		pthread_mutex_unlock(&p->lock);
	}
	if (pts == VIDEO_TIME_NONE && p->audio_first != VIDEO_TIME_NONE)
		pts = p->audio_first;
	if (pts == VIDEO_TIME_NONE && p->video_first != VIDEO_TIME_NONE)
		pts = p->video_first;
	return pts == VIDEO_TIME_NONE ? 0 : pts;
}

static void
vl_run(VideoPlayer *p, int64_t now, int running)
{
	if (running == p->running)
		return;
	if (running) {
		if (!p->clock_known) {
			p->base_pts = vl_initial_pts(p);
			p->clock_known = 1;
		}
		p->base_time = now;
		p->running = 1;
	} else {
		int64_t pos = vl_position(p, now);
		if (pos != VIDEO_TIME_NONE)
			p->base_pts = pos;
		p->running = 0;
	}
	if (p->has_audio)
		vl_sink_run(p, running, now);
}

static int
vl_has_frame(VideoPlayer *p, int nframes)
{
	return (p->current && !p->stale) || nframes > 0;
}

/* Enough media to start: a decoded frame and prebuffer of each stream past
 * the clock (or the seek start, or the stream's first packet). */
static int
vl_ready(VideoPlayer *p, int64_t pos, int nframes, int eos, int vdrained)
{
	int64_t ref;

	if (eos)
		return !p->has_video || vl_has_frame(p, nframes) || vdrained;
	if (p->has_video) {
		if (!vl_has_frame(p, nframes) || p->video_end == VIDEO_TIME_NONE)
			return 0;
		ref = pos != VIDEO_TIME_NONE ? pos : p->start != VIDEO_TIME_NONE ?
		    p->start : p->video_first;
		if (p->video_end - ref < p->prebuffer)
			return 0;
	}
	if (p->has_audio) {
		if (p->audio_end == VIDEO_TIME_NONE)
			return 0;
		ref = pos != VIDEO_TIME_NONE ? pos : p->start != VIDEO_TIME_NONE ?
		    p->start : p->audio_first;
		if (p->audio_end + p->audio_frame_us - ref < p->prebuffer)
			return 0;
	}
	return 1;
}

/* A stream ran out before end of stream: the clock caught up with its
 * newest packet. */
static int
vl_dry(VideoPlayer *p, int64_t pos, int nframes)
{
	if (pos == VIDEO_TIME_NONE)
		return 0;
	if (p->has_video && !nframes &&
	    (p->video_end == VIDEO_TIME_NONE || pos >= p->video_end))
		return 1;
	if (p->has_audio && p->audio_clock &&
	    (p->audio_end == VIDEO_TIME_NONE ||
	    pos + 10000 >= p->audio_end + p->audio_frame_us))
		return 1;
	return 0;
}

static void
vl_update(VideoPlayer *p, int64_t now)
{
	int64_t pos;
	int nframes, eos, vdrained, adrained, audio_done;

	if (__atomic_load_n(&p->sink.failed, __ATOMIC_RELAXED))
		vl_sink_fallback(p, now);
	pthread_mutex_lock(&p->lock);
	p->dropped += p->decode_dropped;
	p->decode_dropped = 0;
	if (!p->error && p->decode_error)
		p->error = p->decode_error;
	eos = p->eos;
	vdrained = p->vdrained;
	adrained = p->adrained;
	pthread_mutex_unlock(&p->lock);
	if (p->state == VIDEO_STATE_ENDED)
		return;

	pos = vl_position(p, now);
	vl_select(p, pos);
	audio_done = !p->has_audio ||
	    (eos && adrained && vl_audio_heard(p, now));
	if (p->has_audio && p->audio_clock && audio_done && p->clock_known) {
		/* The sound ended first: the host clock carries on. */
		p->base_pts = pos;
		p->base_time = now;
		p->audio_clock = 0;
	}
	pthread_mutex_lock(&p->lock);
	nframes = p->nframes;
	pthread_mutex_unlock(&p->lock);
	if (p->state == VIDEO_STATE_BUFFERING && !p->paused &&
	    vl_ready(p, pos, nframes, eos, vdrained)) {
		vl_run(p, now, 1);
		p->state = VIDEO_STATE_PLAYING;
		pos = vl_position(p, now);
		vl_select(p, pos);
		pthread_mutex_lock(&p->lock);
		nframes = p->nframes;
		pthread_mutex_unlock(&p->lock);
	}
	if (p->state != VIDEO_STATE_PLAYING || !p->running)
		return;
	if (eos) {
		if ((!p->has_video || (vdrained && !nframes)) && audio_done) {
			vl_run(p, now, 0);
			p->state = VIDEO_STATE_ENDED;
		}
	} else if (vl_dry(p, pos, nframes)) {
		vl_run(p, now, 0);
		p->state = VIDEO_STATE_BUFFERING;
	}
}

/* Microseconds until this player needs a tick, -1 for none. */
static int64_t
vl_next_due(VideoPlayer *p, int64_t now)
{
	int64_t due = VL_CHECK_US, pos;

	if (p->state != VIDEO_STATE_PLAYING || !p->running)
		return -1;
	pos = vl_position(p, now);
	pthread_mutex_lock(&p->lock);
	if (p->nframes && pos != VIDEO_TIME_NONE)
		due = VL_MIN(due, VL_MAX(0, p->frames[0]->pts - pos));
	pthread_mutex_unlock(&p->lock);
	return due;
}

/* ---- VideoBackend ---- */

static AVCodecContext *
vl_open_decoder(VideoPlayer *p, int video)
{
	const AVCodec *codec = avcodec_find_decoder(video ? AV_CODEC_ID_H264 :
	    AV_CODEC_ID_AAC);
	AVCodecContext *ctx;

	if (!codec || !(ctx = avcodec_alloc_context3(codec)))
		return NULL;
	ctx->pkt_timebase = (AVRational){1, 1000000};
	if (video) {
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);
		int limit = p->config.width * p->config.height > 1920 * 1088 ?
		    8 : 4;
		/* Frame threads add a frame of latency each; the prebuffer
		 * covers a few. */
		ctx->thread_count = (int)VL_MAX(1, VL_MIN(cpus, limit));
		ctx->thread_type = FF_THREAD_FRAME;
	} else {
		ctx->sample_rate = p->config.sample_rate;
		av_channel_layout_default(&ctx->ch_layout, p->config.channels);
		ctx->extradata = av_mallocz(p->config.audio_config_length +
		    AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx->extradata) {
			avcodec_free_context(&ctx);
			return NULL;
		}
		memcpy(ctx->extradata, p->config.audio_config,
		    p->config.audio_config_length);
		ctx->extradata_size = (int)p->config.audio_config_length;
	}
	if (avcodec_open2(ctx, codec, NULL) < 0)
		avcodec_free_context(&ctx);
	return ctx;
}

static void vl_destroy(VideoPlayer *p);

static VideoPlayer *
vl_create(const VideoConfig *config, const char **error)
{
	VideoPlayer *p = calloc(1, sizeof(*p));
	size_t frame_bytes;

	if (!p) {
		*error = "ENOMEM:video player allocation failed";
		return NULL;
	}
	if (vl_fd < 0)
		vl_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	p->config = *config;
	p->has_video = config->video_codec == VIDEO_CODEC_H264;
	p->has_audio = config->audio_codec == AUDIO_CODEC_AAC;
	p->prebuffer = (int64_t)config->prebuffer_ms * 1000;
	p->paused = config->paused;
	p->start = VIDEO_TIME_NONE;
	p->video_first = p->video_end = VIDEO_TIME_NONE;
	p->audio_first = p->audio_end = VIDEO_TIME_NONE;
	p->state = VIDEO_STATE_BUFFERING;
	p->audio_clock = p->has_audio;
	frame_bytes = (size_t)config->width * config->height * 3 / 2 + 1;
	p->frame_cap = (int)VL_MAX(VL_FRAMES_MIN,
	    VL_MIN(VL_FRAMES_MAX, VL_FRAMES_BYTES / frame_bytes));
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->vcond, NULL);
	pthread_cond_init(&p->acond, NULL);
	pthread_mutex_init(&p->ring.lock, NULL);
	pthread_cond_init(&p->ring.cond, NULL);
	p->ring.volume = (float)config->volume;

	if (p->has_video && !(p->vctx = vl_open_decoder(p, 1))) {
		*error = "ENOTSUP:H.264 decoder unavailable";
		goto fail;
	}
	if (p->has_audio) {
		p->ring.rate = config->sample_rate;
		p->ring.channels = config->channels;
		p->ring.capacity = (size_t)config->sample_rate * VL_AUDIO_SECONDS;
		p->audio_frame_us = 1024 * 1000000LL / config->sample_rate;
		p->ring.samples = malloc(p->ring.capacity * p->ring.channels *
		    sizeof(float));
		if (!p->ring.samples) {
			*error = "ENOMEM:audio buffer allocation failed";
			goto fail;
		}
		if (!(p->actx = vl_open_decoder(p, 0))) {
			*error = "ENOTSUP:AAC decoder unavailable";
			goto fail;
		}
		if (vl_null_audio || !vl_pulse_open(p))
			p->sink.pulse = 0;	/* silent, but keeps time */
	}
	if (p->has_video) {
		if (pthread_create(&p->vthread, NULL, vl_video_thread, p)) {
			*error = "EAGAIN:video thread creation failed";
			goto fail;
		}
		p->vthread_started = 1;
	}
	if (p->has_audio) {
		if (pthread_create(&p->athread, NULL, vl_audio_thread, p)) {
			*error = "EAGAIN:audio thread creation failed";
			goto fail;
		}
		p->athread_started = 1;
	}
	p->next = vl_players;
	vl_players = p;
	return p;

fail:
	vl_destroy(p);
	return NULL;
}

static void
vl_destroy(VideoPlayer *p)
{
	VideoPlayer **link;
	int i;

	for (link = &vl_players; *link; link = &(*link)->next)
		if (*link == p) {
			*link = p->next;
			break;
		}
	pthread_mutex_lock(&p->lock);
	p->quit = 1;
	pthread_cond_broadcast(&p->vcond);
	pthread_cond_broadcast(&p->acond);
	pthread_mutex_unlock(&p->lock);
	pthread_mutex_lock(&p->ring.lock);
	p->ring.quit = 1;
	pthread_cond_broadcast(&p->ring.cond);
	pthread_mutex_unlock(&p->ring.lock);
	if (p->vthread_started)
		pthread_join(p->vthread, NULL);
	if (p->athread_started)
		pthread_join(p->athread, NULL);
	vl_pulse_close(p);
	avcodec_free_context(&p->vctx);
	avcodec_free_context(&p->actx);
	vl_queue_clear(&p->vq);
	vl_queue_clear(&p->aq);
	for (i = 0; i < p->nframes; i++)
		av_frame_free(&p->frames[i]);
	av_frame_free(&p->current);
	free(p->ring.samples);
	sws_freeContext(p->rgba_sws);
	free(p->rgba);
	pthread_mutex_destroy(&p->lock);
	pthread_cond_destroy(&p->vcond);
	pthread_cond_destroy(&p->acond);
	pthread_mutex_destroy(&p->ring.lock);
	pthread_cond_destroy(&p->ring.cond);
	free(p);
}

static int
vl_packet(VideoPlayer *p, const VideoPacket *packet, const char **error)
{
	int video = packet->stream == VIDEO_STREAM_VIDEO;
	QPacket *q;

	if (video ? !p->has_video : !p->has_audio) {
		*error = "EINVAL:packet for a stream the video does not have";
		return 0;
	}
	pthread_mutex_lock(&p->lock);
	if (packet->length > VIDEO_QUEUE_MAX - p->queued) {
		pthread_mutex_unlock(&p->lock);
		*error = "ENOBUFS:video queue full";
		return 0;
	}
	pthread_mutex_unlock(&p->lock);
	q = malloc(sizeof(*q) + packet->length + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!q) {
		*error = "ENOMEM:packet allocation failed";
		return 0;
	}
	q->pts = packet->pts;
	q->keyframe = packet->keyframe;
	q->length = packet->length;
	memcpy(q->data, packet->data, packet->length);
	memset(q->data + packet->length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	pthread_mutex_lock(&p->lock);
	vl_queue_push(video ? &p->vq : &p->aq, q);
	p->queued += packet->length;
	pthread_cond_broadcast(video ? &p->vcond : &p->acond);
	pthread_mutex_unlock(&p->lock);
	if (video) {
		if (p->video_end == VIDEO_TIME_NONE || packet->pts > p->video_end)
			p->video_end = packet->pts;
		if (p->video_first == VIDEO_TIME_NONE || packet->pts < p->video_first)
			p->video_first = packet->pts;
	} else {
		if (p->audio_end == VIDEO_TIME_NONE || packet->pts > p->audio_end)
			p->audio_end = packet->pts;
		if (p->audio_first == VIDEO_TIME_NONE || packet->pts < p->audio_first)
			p->audio_first = packet->pts;
	}
	return 1;
}

static void
vl_flush(VideoPlayer *p, int64_t start)
{
	int64_t now = vl_clock();
	int i;

	vl_run(p, now, 0);
	pthread_mutex_lock(&p->lock);
	p->generation++;
	vl_queue_clear(&p->vq);
	vl_queue_clear(&p->aq);
	p->queued = 0;
	for (i = 0; i < p->nframes; i++)
		av_frame_free(&p->frames[i]);
	p->nframes = 0;
	p->eos = p->vdrained = p->adrained = 0;
	p->start = start;
	p->decode_dropped = 0;
	p->decode_error = NULL;
	pthread_cond_broadcast(&p->vcond);
	pthread_cond_broadcast(&p->acond);
	pthread_mutex_lock(&p->ring.lock);
	p->ring.generation = p->generation;
	p->ring.pulled = p->ring.played = p->ring.pushed;
	p->ring.mtail = p->ring.mhead;
	pthread_cond_broadcast(&p->ring.cond);
	pthread_mutex_unlock(&p->ring.lock);
	pthread_mutex_unlock(&p->lock);
	if (p->has_audio)
		vl_sink_flush(p);
	p->state = VIDEO_STATE_BUFFERING;
	p->clock_known = 0;
	p->audio_clock = p->has_audio;
	p->stale = p->current != NULL;
	p->video_first = p->video_end = VIDEO_TIME_NONE;
	p->audio_first = p->audio_end = VIDEO_TIME_NONE;
	p->error = NULL;
}

static void
vl_end(VideoPlayer *p)
{
	pthread_mutex_lock(&p->lock);
	p->eos = 1;
	pthread_cond_broadcast(&p->vcond);
	pthread_cond_broadcast(&p->acond);
	pthread_mutex_unlock(&p->lock);
	vl_update(p, vl_clock());
}

static void
vl_pause(VideoPlayer *p, int paused)
{
	int64_t now = vl_clock();

	vl_update(p, now);
	p->paused = !!paused;
	if (p->paused)
		vl_run(p, now, 0);
	else if (p->state == VIDEO_STATE_PLAYING)
		vl_run(p, now, 1);
	vl_update(p, now);
}

static void
vl_volume(VideoPlayer *p, double volume)
{
	pthread_mutex_lock(&p->ring.lock);
	p->ring.volume = (float)volume;
	pthread_mutex_unlock(&p->ring.lock);
}

static void
vl_status(VideoPlayer *p, VideoStatus *status)
{
	int64_t now = vl_clock();

	vl_update(p, now);
	status->state = p->state == VIDEO_STATE_ENDED ? VIDEO_STATE_ENDED :
	    p->paused ? VIDEO_STATE_PAUSED : p->state;
	status->position = vl_position(p, now);
	if (status->position == VIDEO_TIME_NONE && p->current && !p->stale)
		status->position = p->current->pts;
	status->video_end = p->video_end;
	status->audio_end = p->audio_end;
	status->shown = p->shown;
	status->dropped = p->dropped;
	status->error = p->error;
}

static const VideoBackend vl_backend = {
	"video=h264 audio=aac max=4096x4096",
	vl_create, vl_destroy, vl_packet, vl_flush, vl_end, vl_pause,
	vl_volume, vl_status,
};

const VideoBackend *
video_linux_backend(void)
{
	return &vl_backend;
}

int
video_linux_fd(void)
{
	return vl_fd;
}

double
video_linux_tick(int *changed)
{
	VideoPlayer *p;
	int64_t now = vl_clock(), next = -1, due;
	uint64_t count;

	if (vl_fd >= 0 && read(vl_fd, &count, sizeof(count)) < 0)
		count = 0;
	for (p = vl_players; p; p = p->next) {
		vl_update(p, now);
		if (p->changed) {
			if (changed && p->drawn_epoch == vl_epoch)
				*changed = 1;
			p->changed = 0;
		}
		due = vl_next_due(p, now);
		if (due >= 0 && (next < 0 || due < next))
			next = due;
	}
	/* Round up so the frame is due when the wait ends. */
	return next < 0 ? -1 : (double)(next + 999) / 1000.0;
}

void
video_linux_begin_frame(void)
{
	vl_epoch++;
}

void
video_linux_drawn(VideoPlayer *p)
{
	p->drawn_epoch = vl_epoch;
}

static int
vl_matrix(const AVFrame *frame)
{
	switch (frame->colorspace) {
	case AVCOL_SPC_BT709:
		return VIDEO_MATRIX_BT709;
	case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL:
		return VIDEO_MATRIX_BT2020;
	case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_FCC:
		return VIDEO_MATRIX_BT601;
	default:
		return frame->height > 576 ? VIDEO_MATRIX_BT709 :
		    VIDEO_MATRIX_BT601;
	}
}

int
video_linux_frame(VideoPlayer *p, VideoFrameView *view)
{
	const AVFrame *frame = p->current;
	const AVPixFmtDescriptor *desc;
	int i;

	if (!frame || !(desc = av_pix_fmt_desc_get(frame->format)))
		return 0;
	view->seq = p->seq;
	view->width = frame->width;
	view->height = frame->height;
	for (i = 0; i < 3; i++) {
		int sx = i ? desc->log2_chroma_w : 0, sy = i ? desc->log2_chroma_h : 0;
		view->plane[i] = frame->data[i];
		view->linesize[i] = frame->linesize[i];
		view->plane_width[i] = -((-frame->width) >> sx);
		view->plane_height[i] = -((-frame->height) >> sy);
	}
	view->matrix = vl_matrix(frame);
	view->full_range = frame->color_range == AVCOL_RANGE_JPEG ||
	    frame->format == AV_PIX_FMT_YUVJ420P ||
	    frame->format == AV_PIX_FMT_YUVJ422P ||
	    frame->format == AV_PIX_FMT_YUVJ444P;
	return 1;
}

const unsigned char *
video_linux_frame_rgba(VideoPlayer *p, int *width, int *height, uint64_t *seq)
{
	const AVFrame *frame = p->current;
	size_t size;

	if (!frame)
		return NULL;
	size = (size_t)frame->width * frame->height * 4;
	if (p->rgba_seq != p->seq || !p->rgba) {
		uint8_t *dst[4] = {NULL};
		int dstlinesize[4] = {frame->width * 4};
		int src_range = frame->color_range == AVCOL_RANGE_JPEG;
		if (size > p->rgba_size) {
			unsigned char *grown = realloc(p->rgba, size);
			if (!grown)
				return NULL;
			p->rgba = grown;
			p->rgba_size = size;
		}
		p->rgba_sws = sws_getCachedContext(p->rgba_sws, frame->width,
		    frame->height, frame->format, frame->width, frame->height,
		    AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
		if (!p->rgba_sws)
			return NULL;
		sws_setColorspaceDetails(p->rgba_sws, sws_getCoefficients(
		    vl_matrix(frame) == VIDEO_MATRIX_BT709 ? SWS_CS_ITU709 :
		    vl_matrix(frame) == VIDEO_MATRIX_BT2020 ? SWS_CS_BT2020 :
		    SWS_CS_ITU601), src_range, sws_getCoefficients(SWS_CS_DEFAULT),
		    1, 0, 1 << 16, 1 << 16);
		dst[0] = p->rgba;
		sws_scale(p->rgba_sws, (const uint8_t *const *)frame->data,
		    frame->linesize, 0, frame->height, dst, dstlinesize);
		p->rgba_seq = p->seq;
	}
	*width = frame->width;
	*height = frame->height;
	*seq = p->seq;
	return p->rgba;
}

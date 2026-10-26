/* st-play media pipeline (see media.h). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "media.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define QUEUE_BYTES   (48 * 1024 * 1024)
#define QUEUE_PACKETS 2048
#define GOP_PACKETS   1024
#define US            ((AVRational){ 1, 1000000 })
#define AUDIO_TB      ((AVRational){ 1, MEDIA_AUDIO_RATE })

typedef struct Node {
	AVPacket *packet;
	unsigned gen;
	struct Node *next;
} Node;

struct Media {
	AVFormatContext *fmt;
	int vi, ai;
	int64_t start_time;	/* us */
	MediaInfo info;

	/* Demux thread */
	pthread_t thread;
	int thread_started;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	Node *head, *tail;
	size_t queue_bytes, queue_count;
	unsigned gen;
	int seek_pending;
	int64_t seek_target;
	int eof;
	unsigned eof_gen;
	int read_error;
	volatile int quit;
	volatile sig_atomic_t *cancel;
	int wake[2];

	/* Decode */
	AVCodecContext *vdec, *adec;
	AVFrame *frame, *held, *scaled, *aframe;
	AVPacket *out;
	struct SwsContext *sws;
	int sws_w, sws_h, sws_fmt;
	SwrContext *swr;
	int swr_rate, swr_fmt;
	AVChannelLayout swr_layout;
	AVAudioFifo *fifo;

	/* Encode */
	AVCodecContext *venc, *aenc;
	/* A size change drains the old encoder a packet per step (its delayed
	 * frames take up to a second to encode), then encodes the frame that
	 * caused it with the new one. */
	AVCodecContext *old_venc;
	AVFrame *pending;
	int64_t pending_pts;
	int have_pending;
	int enc_w, enc_h, enc_kbps;
	double enc_fps;
	char enc_preset[16];
	int want_w, want_h, want_kbps;
	double want_fps;
	char want_preset[16];
	int audio_kbps, want_audio_kbps;
	AVBSFContext *bsf;
	int copying, want_copy, copy_started;
	/* Passthrough keeps the packets since the last keyframe, so it can
	 * switch to transcoding at once: decode them, encode what follows. */
	AVPacket *gop[GOP_PACKETS];
	int gop_count;
	int64_t copy_last_pts;

	/* Segment state */
	int64_t video_trim, audio_trim;	/* drop media before (us) */
	int trim_skip;			/* video_trim: drop, don't hold */
	int have_held;
	int64_t held_pts;
	int64_t last_out_pts;		/* decimation */
	int64_t last_enc_pts;
	int64_t audio_base;		/* samples at 48 kHz, MEDIA_TIME_NONE */
	int64_t audio_samples;
	int draining, done;
	char error[256];
};

/* Logging */

static FILE *log_file;
static char last_log[256];

static void
log_callback(void *avcl, int level, const char *fmt, va_list args)
{
	char line[1024];
	int print = 1;

	(void)avcl;
	if (level > AV_LOG_WARNING && !log_file)
		return;
	if (level > AV_LOG_INFO)
		return;
	vsnprintf(line, sizeof(line), fmt, args);
	if (level <= AV_LOG_ERROR) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (n)
			snprintf(last_log, sizeof(last_log), "%s", line);
	}
	if (log_file && print) {
		fputs(line, log_file);
		fflush(log_file);
	}
}

void
media_log_to(const char *path)
{
	if (path)
		log_file = fopen(path, "a");
	av_log_set_callback(log_callback);
}

const char *
media_last_log(void)
{
	return last_log;
}

static void
set_error(Media *m, const char *what, int code)
{
	char buf[128];
	av_strerror(code, buf, sizeof(buf));
	snprintf(m->error, sizeof(m->error), "%s: %s", what, buf);
}

const char *
media_error(const Media *m)
{
	return m->error;
}

/* Demux thread */

static void
wake_main(Media *m)
{
	char c = 1;
	ssize_t r = write(m->wake[1], &c, 1);
	(void)r;
}

static int
interrupt_cb(void *opaque)
{
	Media *m = opaque;
	return m->quit || (m->cancel && *m->cancel);
}

static void
clear_queue(Media *m)
{
	while (m->head) {
		Node *n = m->head;
		m->head = n->next;
		av_packet_free(&n->packet);
		free(n);
	}
	m->tail = NULL;
	m->queue_bytes = m->queue_count = 0;
}

static void *
demux_thread(void *arg)
{
	Media *m = arg;
	AVPacket *packet = av_packet_alloc();

	pthread_mutex_lock(&m->lock);
	while (!m->quit) {
		unsigned gen;
		int r;

		if (m->seek_pending) {
			int64_t target = m->seek_target;
			m->seek_pending = 0;
			pthread_mutex_unlock(&m->lock);
			r = av_seek_frame(m->fmt, -1, target, AVSEEK_FLAG_BACKWARD);
			if (r < 0)
				r = av_seek_frame(m->fmt, -1, target, AVSEEK_FLAG_ANY |
				    AVSEEK_FLAG_BACKWARD);
			pthread_mutex_lock(&m->lock);
			continue;
		}
		if ((m->eof && m->eof_gen == m->gen) ||
		    m->queue_bytes >= QUEUE_BYTES || m->queue_count >= QUEUE_PACKETS) {
			pthread_cond_wait(&m->cond, &m->lock);
			continue;
		}
		gen = m->gen;
		pthread_mutex_unlock(&m->lock);
		r = av_read_frame(m->fmt, packet);
		pthread_mutex_lock(&m->lock);
		if (gen != m->gen || m->seek_pending) {
			if (r >= 0)
				av_packet_unref(packet);
			continue;
		}
		if (r == AVERROR(EAGAIN)) {
			struct timespec ts = { 0, 5000000 };
			pthread_mutex_unlock(&m->lock);
			nanosleep(&ts, NULL);
			pthread_mutex_lock(&m->lock);
			continue;
		}
		if (r < 0) {
			m->eof = 1;
			m->eof_gen = gen;
			m->read_error = r == AVERROR_EOF ? 0 : r;
			wake_main(m);
			continue;
		}
		if (packet->stream_index != m->vi && packet->stream_index != m->ai) {
			av_packet_unref(packet);
			continue;
		}
		{
			Node *n = calloc(1, sizeof(*n));
			if (!n) {
				av_packet_unref(packet);
				continue;
			}
			n->packet = av_packet_alloc();
			av_packet_move_ref(n->packet, packet);
			n->gen = gen;
			if (m->tail)
				m->tail->next = n;
			else
				m->head = n;
			m->tail = n;
			m->queue_bytes += (size_t)n->packet->size + 256;
			m->queue_count++;
			if (m->queue_count == 1)
				wake_main(m);
		}
	}
	pthread_mutex_unlock(&m->lock);
	av_packet_free(&packet);
	return NULL;
}

/* Open */

static AVCodecContext *
open_decoder(AVStream *st, char *error, size_t size)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *c;
	int r;

	if (!codec) {
		snprintf(error, size, "no decoder for %s",
		    avcodec_get_name(st->codecpar->codec_id));
		return NULL;
	}
	c = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(c, st->codecpar);
	c->pkt_timebase = st->time_base;
	c->thread_count = 0;
	if ((r = avcodec_open2(c, codec, NULL)) < 0) {
		char buf[128];
		av_strerror(r, buf, sizeof(buf));
		snprintf(error, size, "opening %s decoder: %s", codec->name, buf);
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

Media *
media_open(const char *url, int want_audio, volatile sig_atomic_t *cancel,
		MediaInfo *info, char *error, size_t error_size)
{
	Media *m = calloc(1, sizeof(*m));
	AVDictionary *opts = NULL;
	int r;

	if (!m) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	m->vi = m->ai = -1;
	m->wake[0] = m->wake[1] = -1;
	m->cancel = cancel;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->cond, NULL);
	m->fmt = avformat_alloc_context();
	m->fmt->interrupt_callback.callback = interrupt_cb;
	m->fmt->interrupt_callback.opaque = m;
	if (!strcmp(url, "-"))
		url = "pipe:0";
	av_dict_set(&opts, "reconnect", "1", 0);
	r = avformat_open_input(&m->fmt, url, NULL, &opts);
	av_dict_free(&opts);
	if (r < 0) {
		char buf[128];
		av_strerror(r, buf, sizeof(buf));
		snprintf(error, error_size, "%s: %s", url, buf);
		m->fmt = NULL;
		media_close(m);
		return NULL;
	}
	if ((r = avformat_find_stream_info(m->fmt, NULL)) < 0) {
		char buf[128];
		av_strerror(r, buf, sizeof(buf));
		snprintf(error, error_size, "%s: %s", url, buf);
		media_close(m);
		return NULL;
	}
	m->vi = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (m->vi >= 0 && (m->fmt->streams[m->vi]->disposition &
	    AV_DISPOSITION_ATTACHED_PIC))
		m->vi = -1;
	if (want_audio)
		m->ai = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1,
		    m->vi >= 0 ? m->vi : -1, NULL, 0);
	if (m->vi < 0) m->vi = -1;
	if (m->ai < 0) m->ai = -1;
	if (m->vi < 0 && m->ai < 0) {
		snprintf(error, error_size, "%s: no video or audio stream", url);
		media_close(m);
		return NULL;
	}
	m->start_time = m->fmt->start_time != AV_NOPTS_VALUE ?
	    m->fmt->start_time : 0;
	memset(&m->info, 0, sizeof(m->info));
	m->info.duration = m->fmt->duration != AV_NOPTS_VALUE ?
	    m->fmt->duration : MEDIA_TIME_NONE;
	m->info.seekable = !m->fmt->pb ||
	    (m->fmt->pb->seekable & AVIO_SEEKABLE_NORMAL);
	if (m->vi >= 0) {
		AVStream *st = m->fmt->streams[m->vi];
		AVCodecParameters *p = st->codecpar;
		AVRational sar = av_guess_sample_aspect_ratio(m->fmt, st, NULL);
		AVRational fr = av_guess_frame_rate(m->fmt, st, NULL);
		if (!(m->vdec = open_decoder(st, error, error_size))) {
			media_close(m);
			return NULL;
		}
		m->info.has_video = 1;
		m->info.width = p->width;
		m->info.height = p->height;
		if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
			m->info.width = (int)((int64_t)p->width * sar.num / sar.den);
		if (m->info.width < 2) m->info.width = 2;
		if (m->info.height < 2) m->info.height = 2;
		m->info.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 30;
		if (m->info.fps > 120 || m->info.fps < 1)
			m->info.fps = 30;
		m->info.copyable = p->codec_id == AV_CODEC_ID_H264 &&
		    (p->format == AV_PIX_FMT_YUV420P ||
		    p->format == AV_PIX_FMT_YUVJ420P) &&
		    p->width <= 4096 && p->height <= 4096;
		m->info.bitrate = p->bit_rate;
		if (!m->info.bitrate && m->fmt->bit_rate > 0)
			m->info.bitrate = m->fmt->bit_rate;
		snprintf(m->info.video_desc, sizeof(m->info.video_desc),
		    "%s %dx%d %.3g fps", avcodec_get_name(p->codec_id),
		    p->width, p->height, m->info.fps);
	}
	if (m->ai >= 0) {
		AVStream *st = m->fmt->streams[m->ai];
		if (!(m->adec = open_decoder(st, error, error_size))) {
			media_close(m);
			return NULL;
		}
		m->info.has_audio = 1;
		snprintf(m->info.audio_desc, sizeof(m->info.audio_desc),
		    "%s %d Hz %d ch", avcodec_get_name(st->codecpar->codec_id),
		    st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels);
	}
	for (unsigned i = 0; i < m->fmt->nb_streams; i++)
		if ((int)i != m->vi && (int)i != m->ai)
			m->fmt->streams[i]->discard = AVDISCARD_ALL;

	m->frame = av_frame_alloc();
	m->held = av_frame_alloc();
	m->aframe = av_frame_alloc();
	m->out = av_packet_alloc();
	m->video_trim = m->audio_trim = MEDIA_TIME_NONE;
	m->last_out_pts = m->last_enc_pts = MEDIA_TIME_NONE;
	m->audio_base = MEDIA_TIME_NONE;
	m->copy_last_pts = MEDIA_TIME_NONE;
	m->audio_kbps = m->want_audio_kbps = 128;
	m->want_fps = m->info.fps;
	if (pipe(m->wake) < 0) {
		snprintf(error, error_size, "pipe: %s", strerror(errno));
		media_close(m);
		return NULL;
	}
	fcntl(m->wake[0], F_SETFL, fcntl(m->wake[0], F_GETFL) | O_NONBLOCK);
	fcntl(m->wake[1], F_SETFL, fcntl(m->wake[1], F_GETFL) | O_NONBLOCK);
	fcntl(m->wake[0], F_SETFD, FD_CLOEXEC);
	fcntl(m->wake[1], F_SETFD, FD_CLOEXEC);
	if (pthread_create(&m->thread, NULL, demux_thread, m)) {
		snprintf(error, error_size, "cannot start demux thread");
		media_close(m);
		return NULL;
	}
	m->thread_started = 1;
	*info = m->info;
	return m;
}

static void free_encoders(Media *m);

static void
clear_gop(Media *m)
{
	while (m->gop_count)
		av_packet_free(&m->gop[--m->gop_count]);
}

void
media_close(Media *m)
{
	if (!m)
		return;
	if (m->thread_started) {
		pthread_mutex_lock(&m->lock);
		m->quit = 1;
		pthread_cond_signal(&m->cond);
		pthread_mutex_unlock(&m->lock);
		pthread_join(m->thread, NULL);
	}
	clear_queue(m);
	clear_gop(m);
	free_encoders(m);
	avcodec_free_context(&m->vdec);
	avcodec_free_context(&m->adec);
	av_frame_free(&m->frame);
	av_frame_free(&m->held);
	av_frame_free(&m->pending);
	av_frame_free(&m->scaled);
	av_frame_free(&m->aframe);
	av_packet_free(&m->out);
	sws_freeContext(m->sws);
	swr_free(&m->swr);
	av_channel_layout_uninit(&m->swr_layout);
	if (m->fifo)
		av_audio_fifo_free(m->fifo);
	av_bsf_free(&m->bsf);
	if (m->fmt)
		avformat_close_input(&m->fmt);
	if (m->wake[0] >= 0) close(m->wake[0]);
	if (m->wake[1] >= 0) close(m->wake[1]);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->cond);
	free(m);
}

int media_wake_fd(const Media *m) { return m->wake[0]; }

void
media_drain_wake(Media *m)
{
	char buf[64];
	while (read(m->wake[0], buf, sizeof(buf)) > 0)
		;
}

/* Settings */

static const char *presets[] = { "medium", "fast", "faster", "veryfast",
    "superfast", "ultrafast" };

const char *
media_preset_for(double pixel_rate, int faster)
{
	int i = pixel_rate <= 12e6 ? 0 : pixel_rate <= 32e6 ? 1 :
	    pixel_rate <= 70e6 ? 2 : pixel_rate <= 140e6 ? 3 : 4;
	i += faster;
	if (i > 5)
		i = 5;
	return presets[i];
}

void
media_set_video(Media *m, int width, int height, double fps, int kbps,
		const char *preset)
{
	m->want_w = width & ~1;
	m->want_h = height & ~1;
	if (m->want_w < 16) m->want_w = 16;
	if (m->want_h < 16) m->want_h = 16;
	m->want_fps = fps > 0 ? fps : m->info.fps;
	m->want_kbps = kbps;
	snprintf(m->want_preset, sizeof(m->want_preset), "%s",
	    preset ? preset : media_preset_for((double)m->want_w * m->want_h *
	    m->want_fps, 0));
}

void media_set_audio_kbps(Media *m, int kbps) { m->want_audio_kbps = kbps; }
void media_set_copy(Media *m, int copy) { m->want_copy = copy && m->info.copyable; }
int media_copying(const Media *m) { return m->copying; }
int media_encoder_kbps(const Media *m) { return m->enc_kbps; }

void
media_encoder_size(const Media *m, int *width, int *height)
{
	if (m->copying) {
		*width = m->fmt->streams[m->vi]->codecpar->width;
		*height = m->fmt->streams[m->vi]->codecpar->height;
	} else {
		*width = m->venc ? m->enc_w : m->want_w;
		*height = m->venc ? m->enc_h : m->want_h;
	}
}

/* Encoders */

static void
free_encoders(Media *m)
{
	avcodec_free_context(&m->venc);
	avcodec_free_context(&m->old_venc);
	avcodec_free_context(&m->aenc);
}

static int
open_video_encoder(Media *m)
{
	const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
	AVCodecContext *c;
	AVDictionary *opts = NULL;
	AVCodecParameters *src = m->fmt->streams[m->vi]->codecpar;
	int r, gop;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);

	if (!codec) {
		snprintf(m->error, sizeof(m->error),
		    "ffmpeg was built without libx264");
		return -1;
	}
	c = avcodec_alloc_context3(codec);
	c->width = m->want_w;
	c->height = m->want_h;
	c->pix_fmt = AV_PIX_FMT_YUV420P;
	c->time_base = US;
	c->framerate = av_d2q(m->want_fps, 1001000);
	c->sample_aspect_ratio = (AVRational){ 1, 1 };
	c->bit_rate = (int64_t)m->want_kbps * 1000;
	c->rc_max_rate = c->bit_rate * 3 / 2;
	c->rc_buffer_size = (int)(c->bit_rate * 2);
	gop = (int)(m->want_fps * 10);
	c->gop_size = gop > 600 ? 600 : gop < 30 ? 30 : gop;
	c->max_b_frames = 3;
	/* Each x264 frame thread and lookahead frame delays the first packet
	 * after a start, seek or resolution change; keep both small. */
	c->thread_count = (double)c->width * c->height * m->want_fps <= 40e6 ? 4 :
	    cpus > 8 ? 8 : cpus > 4 ? (int)cpus : 4;
	c->color_primaries = src->color_primaries;
	c->color_trc = src->color_trc;
	c->colorspace = src->color_space;
	c->color_range = AVCOL_RANGE_MPEG;
	av_dict_set(&opts, "preset", m->want_preset, 0);
	av_dict_set(&opts, "profile", "high", 0);
	av_dict_set(&opts, "forced-idr", "1", 0);
	av_dict_set(&opts, "x264-params",
	    "repeat-headers=1:open-gop=0:rc-lookahead=15", 0);
	r = avcodec_open2(c, codec, &opts);
	av_dict_free(&opts);
	if (r < 0) {
		set_error(m, "opening libx264", r);
		avcodec_free_context(&c);
		return -1;
	}
	m->venc = c;
	m->enc_w = m->want_w;
	m->enc_h = m->want_h;
	m->enc_fps = m->want_fps;
	m->enc_kbps = m->want_kbps;
	snprintf(m->enc_preset, sizeof(m->enc_preset), "%s", m->want_preset);
	return 0;
}

static int
open_audio_encoder(Media *m)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	AVCodecContext *c;
	int r;

	if (!codec) {
		snprintf(m->error, sizeof(m->error), "no AAC encoder");
		return -1;
	}
	c = avcodec_alloc_context3(codec);
	c->sample_fmt = AV_SAMPLE_FMT_FLTP;
	c->sample_rate = MEDIA_AUDIO_RATE;
	av_channel_layout_default(&c->ch_layout, 2);
	c->bit_rate = (int64_t)m->want_audio_kbps * 1000;
	c->time_base = AUDIO_TB;
	c->profile = AV_PROFILE_AAC_LOW;
	if ((r = avcodec_open2(c, codec, NULL)) < 0) {
		set_error(m, "opening AAC encoder", r);
		avcodec_free_context(&c);
		return -1;
	}
	m->aenc = c;
	m->audio_kbps = m->want_audio_kbps;
	if (!m->fifo)
		m->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
	return 0;
}

static int
drain_packets(Media *m, AVCodecContext *c, int stream, MediaEmit emit,
		void *ctx)
{
	int r;

	while ((r = avcodec_receive_packet(c, m->out)) >= 0) {
		int64_t pts = m->out->pts;
		if (pts != AV_NOPTS_VALUE) {
			pts = av_rescale_q(pts, c->time_base, US);
			emit(ctx, stream, !!(m->out->flags & AV_PKT_FLAG_KEY), pts,
			    m->out->data, (size_t)m->out->size);
		}
		av_packet_unref(m->out);
	}
	return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
}

static int encode_video(Media *m, AVFrame *frame, int64_t pts,
		MediaEmit emit, void *ctx);

/* One packet of the old encoder; 0 once it is drained and freed. */
static int
drain_old_step(Media *m, MediaEmit emit, void *ctx)
{
	int r;

	if (!m->old_venc)
		return 0;
	r = avcodec_receive_packet(m->old_venc, m->out);
	if (r >= 0) {
		if (m->out->pts != AV_NOPTS_VALUE)
			emit(ctx, 0, !!(m->out->flags & AV_PKT_FLAG_KEY),
			    av_rescale_q(m->out->pts, m->old_venc->time_base, US),
			    m->out->data, (size_t)m->out->size);
		av_packet_unref(m->out);
		return 1;
	}
	avcodec_free_context(&m->old_venc);
	return 0;
}

/* Finish a switch: drain the old encoder, encode the frame that waited. */
static int
finish_switch(Media *m, MediaEmit emit, void *ctx)
{
	int r = 0;

	while (drain_old_step(m, emit, ctx))
		;
	if (m->have_pending) {
		m->have_pending = 0;
		r = encode_video(m, m->pending, m->pending_pts, emit, ctx);
		av_frame_unref(m->pending);
	}
	return r;
}

static void
flush_video_encoder(Media *m, MediaEmit emit, void *ctx)
{
	finish_switch(m, emit, ctx);
	if (!m->venc)
		return;
	avcodec_send_frame(m->venc, NULL);
	drain_packets(m, m->venc, 0, emit, ctx);
	avcodec_free_context(&m->venc);
}

static int
encode_video(Media *m, AVFrame *frame, int64_t pts, MediaEmit emit, void *ctx)
{
	int r;

	if (m->venc && (m->want_w != m->enc_w || m->want_h != m->enc_h ||
	    m->want_fps != m->enc_fps || strcmp(m->want_preset, m->enc_preset))) {
		/* Start draining; media_step goes on with it. */
		m->old_venc = m->venc;
		m->venc = NULL;
		avcodec_send_frame(m->old_venc, NULL);
		if (!m->pending)
			m->pending = av_frame_alloc();
		av_frame_unref(m->pending);
		if (av_frame_ref(m->pending, frame) < 0)
			return -1;
		m->pending_pts = pts;
		m->have_pending = 1;
		return 0;
	}
	if (!m->venc) {
		if (!m->want_w || open_video_encoder(m) < 0)
			return -1;
		m->last_enc_pts = MEDIA_TIME_NONE;
	} else if (m->want_kbps != m->enc_kbps) {
		/* libx264 reconfigures bitrate and VBV in place. */
		m->venc->bit_rate = (int64_t)m->want_kbps * 1000;
		m->venc->rc_max_rate = m->venc->bit_rate * 3 / 2;
		m->venc->rc_buffer_size = (int)(m->venc->bit_rate * 2);
		m->enc_kbps = m->want_kbps;
	}
	if (m->last_enc_pts != MEDIA_TIME_NONE && pts <= m->last_enc_pts)
		return 0;
	m->last_enc_pts = pts;

	if (!m->sws || m->sws_w != frame->width || m->sws_h != frame->height ||
	    m->sws_fmt != frame->format || !m->scaled ||
	    m->scaled->width != m->enc_w || m->scaled->height != m->enc_h) {
		int full = frame->color_range == AVCOL_RANGE_JPEG ||
		    frame->format == AV_PIX_FMT_YUVJ420P ||
		    frame->format == AV_PIX_FMT_YUVJ422P ||
		    frame->format == AV_PIX_FMT_YUVJ444P;
		int flags = frame->width > m->enc_w * 2 ? SWS_AREA : SWS_BICUBIC;
		sws_freeContext(m->sws);
		/* Scaling runs on the main thread between encoder calls: let
		 * swscale slice it over a few threads. */
		m->sws = sws_alloc_context();
		if (m->sws) {
			const int *coefs = sws_getCoefficients(
			    frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
			    SWS_CS_DEFAULT);
			av_opt_set_int(m->sws, "srcw", frame->width, 0);
			av_opt_set_int(m->sws, "srch", frame->height, 0);
			av_opt_set_int(m->sws, "src_format", frame->format, 0);
			av_opt_set_int(m->sws, "dstw", m->enc_w, 0);
			av_opt_set_int(m->sws, "dsth", m->enc_h, 0);
			av_opt_set_int(m->sws, "dst_format", AV_PIX_FMT_YUV420P, 0);
			av_opt_set_int(m->sws, "sws_flags", flags, 0);
			av_opt_set_int(m->sws, "threads", 4, 0);
			if (sws_init_context(m->sws, NULL, NULL) < 0)
				sws_freeContext(m->sws), m->sws = NULL;
			else
				sws_setColorspaceDetails(m->sws, coefs, full, coefs, 0,
				    0, 1 << 16, 1 << 16);
		}
		if (!m->sws) {
			snprintf(m->error, sizeof(m->error),
			    "cannot scale %s %dx%d",
			    av_get_pix_fmt_name(frame->format), frame->width,
			    frame->height);
			return -1;
		}
		m->sws_w = frame->width;
		m->sws_h = frame->height;
		m->sws_fmt = frame->format;
		av_frame_free(&m->scaled);
		m->scaled = av_frame_alloc();
		m->scaled->format = AV_PIX_FMT_YUV420P;
		m->scaled->width = m->enc_w;
		m->scaled->height = m->enc_h;
		if (av_frame_get_buffer(m->scaled, 0) < 0) {
			snprintf(m->error, sizeof(m->error), "out of memory");
			return -1;
		}
	}
	if (av_frame_make_writable(m->scaled) < 0)
		return -1;
	if (sws_scale_frame(m->sws, m->scaled, frame) < 0) {
		snprintf(m->error, sizeof(m->error), "scaling failed");
		return -1;
	}
	m->scaled->pts = pts;
	m->scaled->pict_type = AV_PICTURE_TYPE_NONE;
	m->scaled->color_primaries = m->venc->color_primaries;
	m->scaled->color_trc = m->venc->color_trc;
	m->scaled->colorspace = m->venc->colorspace;
	m->scaled->color_range = AVCOL_RANGE_MPEG;
	if ((r = avcodec_send_frame(m->venc, m->scaled)) < 0) {
		set_error(m, "encoding video", r);
		return -1;
	}
	return drain_packets(m, m->venc, 0, emit, ctx);
}

static int64_t
frame_time(Media *m, AVFrame *frame, AVStream *st)
{
	int64_t pts = frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		pts = frame->pts;
	if (pts == AV_NOPTS_VALUE)
		return MEDIA_TIME_NONE;
	return av_rescale_q(pts, st->time_base, US) - m->start_time;
}

static int
video_frame(Media *m, AVFrame *frame, MediaEmit emit, void *ctx)
{
	int64_t t = frame_time(m, frame, m->fmt->streams[m->vi]);
	double interval;

	if (t == MEDIA_TIME_NONE) {
		/* No timestamps: count frames at the stream rate. */
		t = m->last_enc_pts == MEDIA_TIME_NONE ? 0 :
		    m->last_enc_pts + (int64_t)(1e6 / m->info.fps);
	}
	if (m->video_trim != MEDIA_TIME_NONE && m->trim_skip) {
		if (t < m->video_trim)
			return 0;
		m->video_trim = MEDIA_TIME_NONE;
		m->trim_skip = 0;
	}
	if (m->video_trim != MEDIA_TIME_NONE) {
		if (t < m->video_trim) {
			av_frame_unref(m->held);
			av_frame_move_ref(m->held, frame);
			m->have_held = 1;
			return 0;
		}
		if (t > m->video_trim && m->have_held) {
			int r = encode_video(m, m->held, m->video_trim, emit, ctx);
			m->last_out_pts = m->video_trim;
			av_frame_unref(m->held);
			m->have_held = 0;
			if (r < 0)
				return r;
		}
		m->video_trim = MEDIA_TIME_NONE;
	}
	/* Decimate to the encoder rate (e.g. 60 -> 30 fps on slow links). */
	interval = 1e6 / (m->want_fps > 0 ? m->want_fps : m->info.fps);
	if (m->last_out_pts != MEDIA_TIME_NONE &&
	    (double)(t - m->last_out_pts) < interval * 0.75)
		return 0;
	m->last_out_pts = t;
	return encode_video(m, frame, t, emit, ctx);
}

static int
receive_video(Media *m, MediaEmit emit, void *ctx)
{
	int r;
	while ((r = avcodec_receive_frame(m->vdec, m->frame)) >= 0) {
		r = video_frame(m, m->frame, emit, ctx);
		av_frame_unref(m->frame);
		if (r < 0)
			return r;
	}
	return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : 0;
}

/* Audio */

static int
encode_audio_fifo(Media *m, int flush, MediaEmit emit, void *ctx)
{
	int frame_size;

	if (!m->aenc && open_audio_encoder(m) < 0)
		return -1;
	frame_size = m->aenc->frame_size ? m->aenc->frame_size : 1024;
	while (av_audio_fifo_size(m->fifo) >= frame_size ||
	    (flush && av_audio_fifo_size(m->fifo) > 0)) {
		int n = av_audio_fifo_size(m->fifo);
		int r;
		if (n > frame_size)
			n = frame_size;
		av_frame_unref(m->aframe);
		m->aframe->nb_samples = n;
		m->aframe->format = AV_SAMPLE_FMT_FLTP;
		m->aframe->sample_rate = MEDIA_AUDIO_RATE;
		av_channel_layout_default(&m->aframe->ch_layout, 2);
		if (av_frame_get_buffer(m->aframe, 0) < 0)
			return -1;
		av_audio_fifo_read(m->fifo, (void **)m->aframe->data, n);
		m->aframe->pts = m->audio_base + m->audio_samples;
		m->audio_samples += n;
		if ((r = avcodec_send_frame(m->aenc, m->aframe)) < 0) {
			set_error(m, "encoding audio", r);
			return -1;
		}
		if (drain_packets(m, m->aenc, 1, emit, ctx) < 0)
			return -1;
	}
	if (flush) {
		avcodec_send_frame(m->aenc, NULL);
		drain_packets(m, m->aenc, 1, emit, ctx);
	}
	return 0;
}

static int
audio_frame(Media *m, AVFrame *frame, MediaEmit emit, void *ctx)
{
	int64_t t = frame_time(m, frame, m->fmt->streams[m->ai]);
	const uint8_t *in[AV_NUM_DATA_POINTERS];
	int skip = 0, count = frame->nb_samples, out_count, i, planar;
	int bytes = av_get_bytes_per_sample(frame->format);
	uint8_t **out = NULL;

	if (!m->swr || m->swr_rate != frame->sample_rate ||
	    m->swr_fmt != frame->format ||
	    av_channel_layout_compare(&m->swr_layout, &frame->ch_layout)) {
		AVChannelLayout stereo;
		swr_free(&m->swr);
		av_channel_layout_default(&stereo, 2);
		if (swr_alloc_set_opts2(&m->swr, &stereo, AV_SAMPLE_FMT_FLTP,
		    MEDIA_AUDIO_RATE, &frame->ch_layout, frame->format,
		    frame->sample_rate, 0, NULL) < 0 || swr_init(m->swr) < 0) {
			snprintf(m->error, sizeof(m->error),
			    "cannot resample audio");
			return -1;
		}
		m->swr_rate = frame->sample_rate;
		m->swr_fmt = frame->format;
		av_channel_layout_uninit(&m->swr_layout);
		av_channel_layout_copy(&m->swr_layout, &frame->ch_layout);
	}
	if (t != MEDIA_TIME_NONE && m->audio_trim != MEDIA_TIME_NONE) {
		int64_t end = t + (int64_t)count * 1000000 / frame->sample_rate;
		if (end <= m->audio_trim)
			return 0;
		if (t < m->audio_trim)
			skip = (int)((m->audio_trim - t) * frame->sample_rate / 1000000);
		if (skip > count)
			skip = count;
	}
	if (m->audio_base == MEDIA_TIME_NONE) {
		int64_t start = t == MEDIA_TIME_NONE ? 0 :
		    t + (int64_t)skip * 1000000 / frame->sample_rate;
		if (m->audio_trim != MEDIA_TIME_NONE && start < m->audio_trim)
			start = m->audio_trim;
		m->audio_base = av_rescale_q(start, US, AUDIO_TB);
		m->audio_samples = 0;
	}
	m->audio_trim = MEDIA_TIME_NONE;
	planar = av_sample_fmt_is_planar(frame->format);
	for (i = 0; i < AV_NUM_DATA_POINTERS; i++)
		in[i] = frame->extended_data[0] ? NULL : NULL;
	if (planar) {
		for (i = 0; i < frame->ch_layout.nb_channels && i < AV_NUM_DATA_POINTERS; i++)
			in[i] = frame->extended_data[i] + (size_t)skip * bytes;
	} else {
		in[0] = frame->extended_data[0] +
		    (size_t)skip * bytes * frame->ch_layout.nb_channels;
	}
	count -= skip;
	out_count = swr_get_out_samples(m->swr, count);
	if (out_count <= 0)
		return 0;
	if (av_samples_alloc_array_and_samples(&out, NULL, 2, out_count,
	    AV_SAMPLE_FMT_FLTP, 0) < 0)
		return -1;
	out_count = swr_convert(m->swr, out, out_count, in, count);
	if (out_count > 0) {
		if (!m->fifo)
			m->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
		av_audio_fifo_write(m->fifo, (void **)out, out_count);
	}
	av_freep(&out[0]);
	av_freep(&out);
	return encode_audio_fifo(m, 0, emit, ctx);
}

static int
receive_audio(Media *m, MediaEmit emit, void *ctx)
{
	int r;
	while ((r = avcodec_receive_frame(m->adec, m->frame)) >= 0) {
		r = audio_frame(m, m->frame, emit, ctx);
		av_frame_unref(m->frame);
		if (r < 0)
			return r;
	}
	return 0;
}

static void
flush_audio(Media *m, MediaEmit emit, void *ctx)
{
	if (m->swr && m->fifo && m->audio_base != MEDIA_TIME_NONE) {
		uint8_t **out = NULL;
		int n = swr_get_out_samples(m->swr, 0);
		if (n > 0 && av_samples_alloc_array_and_samples(&out, NULL, 2, n,
		    AV_SAMPLE_FMT_FLTP, 0) >= 0) {
			n = swr_convert(m->swr, out, n, NULL, 0);
			if (n > 0)
				av_audio_fifo_write(m->fifo, (void **)out, n);
			av_freep(&out[0]);
			av_freep(&out);
		}
	}
	if (m->fifo && m->audio_base != MEDIA_TIME_NONE)
		encode_audio_fifo(m, 1, emit, ctx);
}

/* H.264 passthrough */

static int
copy_packet(Media *m, AVPacket *packet, MediaEmit emit, void *ctx)
{
	AVStream *st = m->fmt->streams[m->vi];
	int r;

	if (!m->bsf) {
		const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
		if (!f || av_bsf_alloc(f, &m->bsf) < 0) {
			m->want_copy = 0;
			return 0;
		}
		avcodec_parameters_copy(m->bsf->par_in, st->codecpar);
		m->bsf->time_base_in = st->time_base;
		if (av_bsf_init(m->bsf) < 0) {
			av_bsf_free(&m->bsf);
			m->want_copy = 0;
			return 0;
		}
	}
	if (!m->copy_started) {
		if (!(packet->flags & AV_PKT_FLAG_KEY))
			return 0;
		m->copy_started = 1;
	}
	if (packet->flags & AV_PKT_FLAG_KEY)
		clear_gop(m);
	if (m->gop_count < GOP_PACKETS)
		m->gop[m->gop_count++] = av_packet_clone(packet);
	if ((r = av_bsf_send_packet(m->bsf, packet)) < 0)
		return 0;
	while (av_bsf_receive_packet(m->bsf, m->out) >= 0) {
		if (m->out->pts != AV_NOPTS_VALUE) {
			int64_t pts = av_rescale_q(m->out->pts, st->time_base, US) -
			    m->start_time;
			if (m->copy_last_pts == MEDIA_TIME_NONE || pts > m->copy_last_pts)
				m->copy_last_pts = pts;
			emit(ctx, 0, !!(m->out->flags & AV_PKT_FLAG_KEY), pts,
			    m->out->data, (size_t)m->out->size);
		}
		av_packet_unref(m->out);
	}
	return 0;
}

/* Leave passthrough before a non-keyframe: decode the GOP so far without
 * encoding it (the terminal has those frames), then transcode on. */
static int
copy_to_transcode(Media *m, MediaEmit emit, void *ctx)
{
	int i;

	avcodec_flush_buffers(m->vdec);
	m->video_trim = m->copy_last_pts == MEDIA_TIME_NONE ? MEDIA_TIME_NONE :
	    m->copy_last_pts + 1;
	m->trim_skip = 1;
	m->last_out_pts = MEDIA_TIME_NONE;
	for (i = 0; i < m->gop_count; i++) {
		int r = avcodec_send_packet(m->vdec, m->gop[i]);
		if (r == AVERROR(EAGAIN)) {
			if (receive_video(m, emit, ctx) < 0)
				return -1;
			avcodec_send_packet(m->vdec, m->gop[i]);
		}
		if (receive_video(m, emit, ctx) < 0)
			return -1;
	}
	clear_gop(m);
	m->copying = 0;
	return 0;
}

/* Drain decoder and encoder before a switch between copy and transcode. */
static void
finish_transcode(Media *m, MediaEmit emit, void *ctx)
{
	avcodec_send_packet(m->vdec, NULL);
	receive_video(m, emit, ctx);
	avcodec_flush_buffers(m->vdec);
	flush_video_encoder(m, emit, ctx);
}

static int
video_packet(Media *m, AVPacket *packet, MediaEmit emit, void *ctx)
{
	int r;

	if (m->copying && !m->want_copy && !(packet->flags & AV_PKT_FLAG_KEY) &&
	    m->gop_count > 0 && m->gop_count < GOP_PACKETS) {
		if (copy_to_transcode(m, emit, ctx) < 0)
			return -1;
	} else if (m->want_copy != m->copying && (packet->flags & AV_PKT_FLAG_KEY)) {
		if (m->want_copy) {
			finish_transcode(m, emit, ctx);
			m->copy_started = 0;
		} else {
			avcodec_flush_buffers(m->vdec);
			clear_gop(m);
		}
		m->copying = m->want_copy;
	}
	if (m->copying) {
		if (packet->pts == AV_NOPTS_VALUE) {
			/* Can't time it: transcode instead. */
			m->want_copy = m->copying = 0;
			m->info.copyable = 0;
		} else {
			return copy_packet(m, packet, emit, ctx);
		}
	}
	r = avcodec_send_packet(m->vdec, packet);
	if (r == AVERROR(EAGAIN)) {
		if (receive_video(m, emit, ctx) < 0)
			return -1;
		r = avcodec_send_packet(m->vdec, packet);
	}
	if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA)
		return 0;	/* skip a bad packet */
	return receive_video(m, emit, ctx);
}

static int
audio_packet(Media *m, AVPacket *packet, MediaEmit emit, void *ctx)
{
	int r = avcodec_send_packet(m->adec, packet);
	if (r == AVERROR(EAGAIN)) {
		if (receive_audio(m, emit, ctx) < 0)
			return -1;
		avcodec_send_packet(m->adec, packet);
	}
	return receive_audio(m, emit, ctx);
}

static void
drain_all(Media *m, MediaEmit emit, void *ctx)
{
	if (m->vdec) {
		if (m->copying) {
			if (m->bsf) {
				av_bsf_send_packet(m->bsf, NULL);
				while (av_bsf_receive_packet(m->bsf, m->out) >= 0) {
					AVStream *st = m->fmt->streams[m->vi];
					if (m->out->pts != AV_NOPTS_VALUE)
						emit(ctx, 0, !!(m->out->flags &
						    AV_PKT_FLAG_KEY), av_rescale_q(
						    m->out->pts, st->time_base, US) -
						    m->start_time, m->out->data,
						    (size_t)m->out->size);
					av_packet_unref(m->out);
				}
			}
		} else {
			avcodec_send_packet(m->vdec, NULL);
			receive_video(m, emit, ctx);
			if (m->have_held) {
				encode_video(m, m->held, m->video_trim !=
				    MEDIA_TIME_NONE ? m->video_trim : m->held_pts,
				    emit, ctx);
				av_frame_unref(m->held);
				m->have_held = 0;
			}
			flush_video_encoder(m, emit, ctx);
		}
	}
	if (m->adec) {
		avcodec_send_packet(m->adec, NULL);
		receive_audio(m, emit, ctx);
		flush_audio(m, emit, ctx);
		avcodec_free_context(&m->aenc);
	}
}

int
media_step(Media *m, MediaEmit emit, void *ctx)
{
	Node *node = NULL;
	int eof = 0, r = 0;

	if (m->done)
		return MEDIA_EOF;
	if (m->error[0])
		return MEDIA_ERROR;
	if (m->old_venc || m->have_pending) {
		/* A switch in progress: one packet per step, then the frame
		 * that waited (opening the new encoder). */
		if (drain_old_step(m, emit, ctx))
			return MEDIA_MORE;
		if (finish_switch(m, emit, ctx) < 0)
			return MEDIA_ERROR;
		return MEDIA_MORE;
	}
	pthread_mutex_lock(&m->lock);
	while (m->head && m->head->gen != m->gen) {
		Node *stale = m->head;
		m->head = stale->next;
		if (!m->head)
			m->tail = NULL;
		m->queue_bytes -= (size_t)stale->packet->size + 256;
		m->queue_count--;
		av_packet_free(&stale->packet);
		free(stale);
	}
	if (m->head) {
		node = m->head;
		m->head = node->next;
		if (!m->head)
			m->tail = NULL;
		m->queue_bytes -= (size_t)node->packet->size + 256;
		m->queue_count--;
		pthread_cond_signal(&m->cond);
	} else if (m->eof && m->eof_gen == m->gen) {
		eof = 1;
		if (m->read_error && !m->error[0])
			set_error(m, "reading input", m->read_error);
	}
	pthread_mutex_unlock(&m->lock);

	if (eof) {
		drain_all(m, emit, ctx);
		m->done = 1;
		return MEDIA_EOF;
	}
	if (!node)
		return MEDIA_WAIT;
	if (node->packet->stream_index == m->vi)
		r = video_packet(m, node->packet, emit, ctx);
	else if (node->packet->stream_index == m->ai)
		r = audio_packet(m, node->packet, emit, ctx);
	av_packet_free(&node->packet);
	free(node);
	if (r < 0) {
		if (!m->error[0])
			snprintf(m->error, sizeof(m->error), "%s",
			    media_last_log()[0] ? media_last_log() : "media error");
		return MEDIA_ERROR;
	}
	return MEDIA_MORE;
}

void
media_seek(Media *m, int64_t target)
{
	if (target < 0)
		target = 0;
	pthread_mutex_lock(&m->lock);
	m->gen++;
	m->seek_pending = 1;
	m->seek_target = target + m->start_time;
	clear_queue(m);
	m->eof = 0;
	pthread_cond_signal(&m->cond);
	pthread_mutex_unlock(&m->lock);

	if (m->vdec)
		avcodec_flush_buffers(m->vdec);
	if (m->adec)
		avcodec_flush_buffers(m->adec);
	free_encoders(m);
	av_frame_unref(m->pending);
	m->have_pending = 0;
	if (m->bsf)
		av_bsf_flush(m->bsf);
	m->copying = m->want_copy;
	m->copy_started = 0;
	clear_gop(m);
	m->copy_last_pts = MEDIA_TIME_NONE;
	m->trim_skip = 0;
	if (m->fifo)
		av_audio_fifo_reset(m->fifo);
	swr_free(&m->swr);
	av_frame_unref(m->held);
	m->have_held = 0;
	m->video_trim = m->audio_trim = target;
	m->held_pts = target;
	m->last_out_pts = m->last_enc_pts = MEDIA_TIME_NONE;
	m->audio_base = MEDIA_TIME_NONE;
	m->audio_samples = 0;
	m->done = 0;
	m->error[0] = '\0';
}

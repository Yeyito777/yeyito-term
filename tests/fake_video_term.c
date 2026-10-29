/* A fake terminal for st-play end-to-end runs without a GUI.
 *
 * Runs a command (st-play) on a PTY and parses what it writes with st's
 * real graphics.c/video.c, behind a link with a configurable bandwidth,
 * round-trip time and buffer: bytes from the program are serialized at the
 * link rate and delivered one-way-delay later; replies and injected keys
 * travel back after the same delay.  The video backend is a timing model:
 * the clock advances with wall time while playing, a stream that runs dry
 * rebuffers for `prebuffer`, and packets are optionally decoded with
 * libavcodec to check that the stream is valid.
 *
 *   fake_video_term [options] -- st-play [args] FILE
 *     --cols N --rows N --cw PX --ch PX   geometry (default 120x40, 10x20)
 *     --bw KBIT          link rate (0: unlimited, default)
 *     --bw-at T:KBIT     change the rate at T seconds (repeatable)
 *     --rtt MS           round-trip time (default 0)
 *     --buffer BYTES     bytes the link may hold before the PTY backs up
 *     --key T:NAME       press a key at T (space left right up down pgup
 *                        pgdn q esc m 9 0 ctrl-c); repeatable
 *     --resize-at T:COLSxROWS
 *     --timeout S        kill the command after S seconds (default 60)
 *     --signal-at T:SIG  send a signal to the command at T
 *     --no-video         no video backend (a terminal without st video)
 *     --no-graphics      ignore graphics APCs (a terminal without Kitty
 *                        graphics, which answers only DA1)
 *     --decode           decode packets with libavcodec and count errors
 *     --pipe             run the command on pipes, as st-play --pipe's
 *                        owner would: its stdout is the terminal's input,
 *                        replies go to its stdin, and --key names become
 *                        pipe commands (space: toggle, left/right: skip
 *                        -5/5, up/down: skip 60/-60, q/esc: quit)
 *     --command T:TEXT   send the pipe command TEXT at T ("seek 4",
 *                        "size 320x180", ...); repeatable
 *     --trace            print one line per second
 * FAKE_TERM_DUMP=FILE saves the video packets received (u32 length, i64
 * pts, data; host order); st-play's STPLAY_DUMP saves those it sent.
 * FAKE_TERM_STDERR=FILE keeps the command's stderr.
 * Prints key=value results on stdout. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../graphics.h"
#include "../video.h"
#include "../play/selectpoll.h"

#ifdef FAKE_DECODE
#include <libavcodec/avcodec.h>
#endif

#define MAX_EVENTS 64

static double t0;
static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
static double rel(void) { return now_s() - t0; }

/* Options */
static int cols = 120, rows = 40, cw = 10, ch = 20;
static double bw_kbit, rtt_ms, timeout_s = 60;
static double link_buffer = 4 * 1024 * 1024;
static int decode, trace, no_video, no_graphics, pipe_mode;
static double signal_at = -1;
static int signal_number;
static struct { double t; double kbit; } bw_events[MAX_EVENTS];
static int nbw;
static struct { double t; char name[32]; int done; } key_events[MAX_EVENTS];
static int nkeys;
static struct { double t; int cols, rows; int done; } resize_events[MAX_EVENTS];
static int nresize;

/* Link */
typedef struct Chunk {
	struct Chunk *next;
	double at;
	size_t length;
	unsigned char data[];
} Chunk;
typedef struct {
	Chunk *head, *tail;
	size_t bytes;
} Fifo;
static Fifo down, up;
static double link_free;

static void
fifo_push(Fifo *f, double at, const void *data, size_t length)
{
	Chunk *c = malloc(sizeof(*c) + length);
	c->next = NULL;
	c->at = at;
	c->length = length;
	memcpy(c->data, data, length);
	if (f->tail)
		f->tail->next = c;
	else
		f->head = c;
	f->tail = c;
	f->bytes += length;
}

static Chunk *
fifo_pop(Fifo *f)
{
	Chunk *c = f->head;
	if (!c)
		return NULL;
	f->head = c->next;
	if (!f->head)
		f->tail = NULL;
	f->bytes -= c->length;
	return c;
}

/* Metrics */
static double first_playing = -1, first_open = -1;
static unsigned long long wire_bytes, payload_bytes, video_bytes;
static unsigned long long sec_wire, sec_video;
static int rebuffers, decode_errors, key_violations, packets_after_end;
static int reply_blocked;
static int flushes, opens, destroys;
static int alt_screen, cursor_hidden;
static double last_rate_sample_t;
static struct { double t; int w, h; } sizes[256];
static int nsizes;
static int max_videos;
/* steady-state windows */
static double window_rates[4096];
static double window_video[4096];
static int window_states[4096];
static int nwindows;
static int window_rebuffered[4096];

typedef struct {
	char name[16];
	double pressed;
	double effect;		/* pause/flush/destroy reached the terminal */
	double playing;		/* playing again after a seek */
} KeyMetric;
static KeyMetric key_metrics[MAX_EVENTS];
static int nkey_metrics;
static int pending_seek_metric = -1;
static int pending_pause_metric = -1;
static int pending_quit_metric = -1;

/* Mock backend */
#define MAX_FRAMES 4096
struct VideoPlayer {
	VideoConfig config;
	int state;
	int paused, ended;
	int clock_set;
	int64_t clock;		/* us */
	double wall;		/* wall time of clock while playing */
	int64_t start;
	int64_t vend, aend;
	int64_t frames[MAX_FRAMES];
	int nframes;
	int have_frame;
	int shown_any;
	uint64_t shown, dropped;
	int need_key;
	int after_flush;
	const char *error;
#ifdef FAKE_DECODE
	AVCodecContext *vdec, *adec;
	AVPacket *pkt;
	AVFrame *frame;
#endif
	int width, height;
};
static VideoPlayer *the_player;

#ifdef FAKE_DECODE
static void
decoder_setup(VideoPlayer *p)
{
	const AVCodec *c;
	p->pkt = av_packet_alloc();
	p->frame = av_frame_alloc();
	if (p->config.video_codec) {
		c = avcodec_find_decoder(AV_CODEC_ID_H264);
		p->vdec = avcodec_alloc_context3(c);
		p->vdec->thread_count = 2;
		avcodec_open2(p->vdec, c, NULL);
	}
	if (p->config.audio_codec) {
		c = avcodec_find_decoder(AV_CODEC_ID_AAC);
		p->adec = avcodec_alloc_context3(c);
		p->adec->extradata = av_mallocz(p->config.audio_config_length +
		    AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(p->adec->extradata, p->config.audio_config,
		    p->config.audio_config_length);
		p->adec->extradata_size = (int)p->config.audio_config_length;
		avcodec_open2(p->adec, c, NULL);
	}
}

static void
decoder_free(VideoPlayer *p)
{
	avcodec_free_context(&p->vdec);
	avcodec_free_context(&p->adec);
	av_packet_free(&p->pkt);
	av_frame_free(&p->frame);
}
#endif

static int64_t
available_end(VideoPlayer *p)
{
	int64_t end = INT64_MAX;
	if (p->config.video_codec)
		end = p->vend == VIDEO_TIME_NONE ? INT64_MIN : p->vend + 33000;
	if (p->config.audio_codec) {
		int64_t a = p->aend == VIDEO_TIME_NONE ? INT64_MIN : p->aend;
		if (a < end)
			end = a;
	}
	return end;
}

static void
player_update(VideoPlayer *p)
{
	double now = now_s();
	int64_t end = available_end(p);

	if (p->state == VIDEO_STATE_PLAYING) {
		p->clock += (int64_t)((now - p->wall) * 1e6);
		p->wall = now;
		if (p->clock >= end) {
			if (p->ended) {
				p->clock = end;
				p->state = VIDEO_STATE_ENDED;
			} else {
				p->clock = end;
				p->state = VIDEO_STATE_BUFFERING;
				rebuffers++;
			}
		}
	}
	if (p->state == VIDEO_STATE_BUFFERING && !p->paused && p->clock_set &&
	    (p->have_frame || !p->config.video_codec) &&
	    (end != INT64_MIN && (end - p->clock >= p->config.prebuffer_ms * 1000LL ||
	    p->ended))) {
		p->state = VIDEO_STATE_PLAYING;
		p->wall = now;
		if (first_playing < 0)
			first_playing = rel();
		if (p->after_flush && pending_seek_metric >= 0) {
			key_metrics[pending_seek_metric].playing = rel();
			pending_seek_metric = -1;
		}
		p->after_flush = 0;
	}
	/* Present frames that are due. */
	{
		int i, due = 0;
		for (i = 0; i < p->nframes; i++)
			if (p->frames[i] <= p->clock)
				due++;
		if (due && p->state != VIDEO_STATE_BUFFERING) {
			p->shown++;
			p->dropped += (uint64_t)due - 1;
			p->shown_any = 1;
		} else if (due) {
			/* buffering: keep only the newest due frame shown */
			p->shown_any = 1;
		}
		if (due) {
			int k = 0;
			for (i = 0; i < p->nframes; i++)
				if (p->frames[i] > p->clock)
					p->frames[k++] = p->frames[i];
			p->nframes = k;
		}
	}
}

static VideoPlayer *
mock_create(const VideoConfig *config, const char **error)
{
	VideoPlayer *p = calloc(1, sizeof(*p));
	(void)error;
	p->config = *config;
	p->paused = config->paused;
	p->state = p->paused ? VIDEO_STATE_PAUSED : VIDEO_STATE_BUFFERING;
	p->vend = p->aend = VIDEO_TIME_NONE;
	p->start = VIDEO_TIME_NONE;
	p->need_key = 1;
	the_player = p;
	opens++;
	if (first_open < 0)
		first_open = rel();
#ifdef FAKE_DECODE
	if (decode)
		decoder_setup(p);
#endif
	return p;
}

static void
mock_destroy(VideoPlayer *p)
{
	if (the_player == p)
		the_player = NULL;
	destroys++;
	if (pending_quit_metric >= 0 && key_metrics[pending_quit_metric].effect < 0)
		key_metrics[pending_quit_metric].effect = rel();
#ifdef FAKE_DECODE
	decoder_free(p);
#endif
	free(p);
}

static int
nal_has(const unsigned char *d, size_t n, int type)
{
	size_t i;
	for (i = 0; i + 3 < n; i++)
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 &&
		    (d[i + 3] & 0x1f) == type)
			return 1;
	return 0;
}

static void
note_size(int w, int h)
{
	if (nsizes && sizes[nsizes - 1].w == w && sizes[nsizes - 1].h == h)
		return;
	if (nsizes < 256) {
		sizes[nsizes].t = rel();
		sizes[nsizes].w = w;
		sizes[nsizes].h = h;
		nsizes++;
	}
}

static int
mock_packet(VideoPlayer *p, const VideoPacket *packet, const char **error)
{
	(void)error;
	player_update(p);
	payload_bytes += packet->length;
	if (p->ended)
		packets_after_end++;
	if (!p->clock_set) {
		p->clock = p->start != VIDEO_TIME_NONE ? p->start : packet->pts;
		p->clock_set = 1;
	}
	if (packet->stream == VIDEO_STREAM_VIDEO) {
		static FILE *dump;
		if (!dump && getenv("FAKE_TERM_DUMP"))
			dump = fopen(getenv("FAKE_TERM_DUMP"), "wb");
		if (dump) {
			uint32_t len = (uint32_t)packet->length;
			int64_t pts = packet->pts;
			fwrite(&len, 4, 1, dump);
			fwrite(&pts, 8, 1, dump);
			fwrite(packet->data, 1, packet->length, dump);
		}
		video_bytes += packet->length;
		sec_video += packet->length;
		if (p->need_key) {
			if (!packet->keyframe || !nal_has(packet->data,
			    packet->length, 7) || !nal_has(packet->data,
			    packet->length, 8))
				key_violations++;
			p->need_key = 0;
		}
		if (p->vend == VIDEO_TIME_NONE || packet->pts > p->vend)
			p->vend = packet->pts;
		if (p->start == VIDEO_TIME_NONE || packet->pts >= p->start) {
			if (p->nframes < MAX_FRAMES)
				p->frames[p->nframes++] = packet->pts;
			p->have_frame = 1;
		} else {
			p->dropped++;
		}
#ifdef FAKE_DECODE
		if (p->vdec) {
			int r;
			av_new_packet(p->pkt, (int)packet->length);
			memcpy(p->pkt->data, packet->data, packet->length);
			p->pkt->pts = packet->pts;
			if (avcodec_send_packet(p->vdec, p->pkt) < 0)
				decode_errors++;
			av_packet_unref(p->pkt);
			while ((r = avcodec_receive_frame(p->vdec, p->frame)) >= 0) {
				if (p->frame->decode_error_flags)
					decode_errors++;
				p->width = p->frame->width;
				p->height = p->frame->height;
				note_size(p->width, p->height);
				av_frame_unref(p->frame);
			}
			if (r != AVERROR(EAGAIN) && r != AVERROR_EOF)
				decode_errors++;
		}
#endif
	} else {
		int64_t end = packet->pts + 1024 * 1000000LL / 48000;
		if (p->aend == VIDEO_TIME_NONE || end > p->aend)
			p->aend = end;
#ifdef FAKE_DECODE
		if (p->adec) {
			av_new_packet(p->pkt, (int)packet->length);
			memcpy(p->pkt->data, packet->data, packet->length);
			if (avcodec_send_packet(p->adec, p->pkt) < 0)
				decode_errors++;
			av_packet_unref(p->pkt);
			while (avcodec_receive_frame(p->adec, p->frame) >= 0)
				av_frame_unref(p->frame);
		}
#endif
	}
	return 1;
}

static void
mock_flush(VideoPlayer *p, int64_t start)
{
	flushes++;
	p->nframes = 0;
	p->have_frame = 0;
	p->vend = p->aend = VIDEO_TIME_NONE;
	p->start = start;
	p->clock_set = 0;
	p->ended = 0;
	p->need_key = 1;
	p->after_flush = 1;
	p->state = p->paused ? VIDEO_STATE_PAUSED : VIDEO_STATE_BUFFERING;
	if (pending_seek_metric >= 0 && key_metrics[pending_seek_metric].effect < 0)
		key_metrics[pending_seek_metric].effect = rel();
#ifdef FAKE_DECODE
	if (p->vdec)
		avcodec_flush_buffers(p->vdec);
	if (p->adec)
		avcodec_flush_buffers(p->adec);
#endif
}

static void
mock_end(VideoPlayer *p)
{
	player_update(p);
	p->ended = 1;
}

static void
mock_pause(VideoPlayer *p, int paused)
{
	player_update(p);
	if (paused == p->paused)
		return;
	p->paused = paused;
	if (paused) {
		p->state = VIDEO_STATE_PAUSED;
	} else {
		p->state = VIDEO_STATE_BUFFERING;
		p->wall = now_s();
		player_update(p);
	}
	if (pending_pause_metric >= 0 && key_metrics[pending_pause_metric].effect < 0) {
		key_metrics[pending_pause_metric].effect = rel();
		pending_pause_metric = -1;
	}
}

static void mock_volume(VideoPlayer *p, double v) { (void)p; (void)v; }

static void
mock_status(VideoPlayer *p, VideoStatus *s)
{
	player_update(p);
	s->state = p->state;
	s->position = p->clock_set && (p->shown_any || !p->config.video_codec) ?
	    p->clock : VIDEO_TIME_NONE;
	s->video_end = p->vend;
	s->audio_end = p->aend;
	s->shown = p->shown;
	s->dropped = p->dropped;
}

static const VideoBackend backend = {
	"video=h264 audio=aac max=4096x4096",
	mock_create, mock_destroy, mock_packet, mock_flush, mock_end,
	mock_pause, mock_volume, mock_status,
};

/* Terminal parser */
static Glyph anchor_line[512];
static int
line_row(Line line, int alt)
{
	(void)alt;
	return line == anchor_line ? 0 : INT_MIN;
}

enum { G_GROUND, G_ESC, G_CSI, G_APC, G_APC_ESC, G_STR, G_STR_ESC };
static int pstate;
static char *apc;
static size_t apc_len, apc_cap;
static char csi[64];
static size_t csi_len;

static unsigned long replies;

static void
reply(const char *data, size_t length)
{
	replies++;
	fifo_push(&up, now_s() + rtt_ms / 2000.0, data, length);
}

static void
handle_csi(void)
{
	char final = csi[csi_len - 1];
	int priv = csi[0] == '?';
	int args[8] = { 0 }, n = 0;
	char *p = csi + (priv ? 1 : 0);

	while (*p && n < 8 && p < csi + csi_len - 1) {
		args[n++] = (int)strtol(p, &p, 10);
		if (*p == ';')
			p++;
		else
			break;
	}
	if (priv && (final == 'h' || final == 'l')) {
		int i;
		for (i = 0; i < n; i++) {
			if (args[i] == 1049)
				alt_screen = final == 'h';
			if (args[i] == 25)
				cursor_hidden = final == 'l';
		}
	} else if (!priv && final == 'c') {
		reply("\033[?62;22c", 9);
	} else if (!priv && final == 't' && n >= 1) {
		char buf[64];
		int k = 0;
		if (args[0] == 16)
			k = snprintf(buf, sizeof(buf), "\033[6;%d;%dt", ch, cw);
		else if (args[0] == 14)
			k = snprintf(buf, sizeof(buf), "\033[4;%d;%dt", rows * ch,
			    cols * cw);
		if (k > 0)
			reply(buf, (size_t)k);
	}
}

static void
handle_apc(void)
{
	GraphicsCommandResult result;
	size_t vc;

	if (!apc_len || apc[0] != 'G' || no_graphics)
		return;
	memset(&result, 0, sizeof(result));
	graphics_handle_apc(apc, apc_len, anchor_line, 0, alt_screen, cw, ch,
	    rows, line_row, 1, &result);
	if (result.response_len)
		reply(result.response, result.response_len);
	vc = graphics_video_count();
	if ((int)vc > max_videos)
		max_videos = (int)vc;
}

static void
terminal_feed(const unsigned char *d, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned char c = d[i];
		switch (pstate) {
		case G_GROUND:
			if (c == 0x1b)
				pstate = G_ESC;
			break;
		case G_ESC:
			if (c == '[') {
				pstate = G_CSI;
				csi_len = 0;
			} else if (c == '_') {
				pstate = G_APC;
				apc_len = 0;
			} else if (c == ']' || c == 'P' || c == '^' || c == 'X') {
				pstate = G_STR;
			} else {
				pstate = G_GROUND;
			}
			break;
		case G_CSI:
			if (csi_len < sizeof(csi) - 1)
				csi[csi_len++] = (char)c;
			if (c >= 0x40 && c <= 0x7e) {
				csi[csi_len] = '\0';
				handle_csi();
				pstate = G_GROUND;
			}
			break;
		case G_APC: {
			/* Bulk copy up to the next ESC. */
			const unsigned char *e = memchr(d + i, 0x1b, n - i);
			size_t take = e ? (size_t)(e - (d + i)) : n - i;
			if (apc_len + take + 1 > apc_cap) {
				apc_cap = (apc_len + take + 1) * 2;
				apc = realloc(apc, apc_cap);
			}
			memcpy(apc + apc_len, d + i, take);
			apc_len += take;
			i += take;
			if (e)
				pstate = G_APC_ESC;
			break;
		}
		case G_APC_ESC:
			if (c == '\\') {
				handle_apc();
				pstate = G_GROUND;
			} else {
				pstate = G_ESC;
				i--;
			}
			break;
		case G_STR:
			if (c == 0x1b)
				pstate = G_STR_ESC;
			else if (c == 7)
				pstate = G_GROUND;
			break;
		case G_STR_ESC:
			pstate = c == '\\' ? G_GROUND : G_STR;
			break;
		}
	}
}

/* In --pipe mode keys are the owner's commands to st-play. */
static const char *
command_bytes(const char *name)
{
	static char buf[64];
	const char *text = name;

	if (!strcmp(name, "space") || !strcmp(name, "p")) text = "toggle";
	else if (!strcmp(name, "left")) text = "skip -5";
	else if (!strcmp(name, "right")) text = "skip 5";
	else if (!strcmp(name, "up")) text = "skip 60";
	else if (!strcmp(name, "down")) text = "skip -60";
	else if (!strcmp(name, "q") || !strcmp(name, "esc")) text = "quit";
	snprintf(buf, sizeof(buf), "\033_!%s\033\\", text);
	return buf;
}

static const char *
key_bytes(const char *name)
{
	if (pipe_mode)
		return command_bytes(name);
	if (!strcmp(name, "space")) return " ";
	if (!strcmp(name, "left")) return "\033[D";
	if (!strcmp(name, "right")) return "\033[C";
	if (!strcmp(name, "up")) return "\033[A";
	if (!strcmp(name, "down")) return "\033[B";
	if (!strcmp(name, "pgup")) return "\033[5~";
	if (!strcmp(name, "pgdn")) return "\033[6~";
	if (!strcmp(name, "esc")) return "\033";
	if (!strcmp(name, "ctrl-c")) return "\003";
	return name;
}

static void
press(int i)
{
	const char *name = key_events[i].name, *b = key_bytes(name);
	KeyMetric *m;

	reply(b, strlen(b));
	if (nkey_metrics >= MAX_EVENTS)
		return;
	m = &key_metrics[nkey_metrics];
	snprintf(m->name, sizeof(m->name), "%s", name);
	m->pressed = rel();
	m->effect = m->playing = -1;
	if (!strcmp(name, "space") || !strcmp(name, "p") ||
	    !strcmp(name, "pause") || !strcmp(name, "resume") ||
	    !strcmp(name, "toggle"))
		pending_pause_metric = nkey_metrics;
	else if (!strcmp(name, "left") || !strcmp(name, "right") ||
	    !strcmp(name, "up") || !strcmp(name, "down") ||
	    !strcmp(name, "pgup") || !strcmp(name, "pgdn") ||
	    !strncmp(name, "seek", 4) || !strncmp(name, "skip", 4))
		pending_seek_metric = nkey_metrics;
	else if (!strcmp(name, "q") || !strcmp(name, "esc") ||
	    !strcmp(name, "ctrl-c") || !strcmp(name, "quit"))
		pending_quit_metric = nkey_metrics;
	nkey_metrics++;
}

static double
current_bw(double t)
{
	double kbit = bw_kbit;
	int i;
	for (i = 0; i < nbw; i++)
		if (bw_events[i].t <= t)
			kbit = bw_events[i].kbit;
	return kbit;
}

static int
parse_time_arg(const char *s, double *t, const char **rest)
{
	char *end;
	*t = strtod(s, &end);
	if (*end != ':')
		return 0;
	*rest = end + 1;
	return 1;
}

int
main(int argc, char **argv)
{
	int master, i, status = 0, exited = 0, rfd, wfd;
	pid_t pid;
	char *slave_name;
	struct winsize ws;
	double child_exit = -1, last_sec = 0, killed_at = -1;
	int timed_out = 0;
	unsigned long long stray_after_exit = 0;
	struct termios final_termios;
	int have_final_termios = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "", *rest;
		double t;
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		if (!strcmp(a, "--decode")) { decode = 1; continue; }
		if (!strcmp(a, "--trace")) { trace = 1; continue; }
		if (!strcmp(a, "--no-video")) { no_video = 1; continue; }
		if (!strcmp(a, "--no-graphics")) { no_graphics = 1; continue; }
		if (!strcmp(a, "--pipe")) { pipe_mode = 1; continue; }
		i++;
		if (!strcmp(a, "--cols")) cols = atoi(v);
		else if (!strcmp(a, "--rows")) rows = atoi(v);
		else if (!strcmp(a, "--cw")) cw = atoi(v);
		else if (!strcmp(a, "--ch")) ch = atoi(v);
		else if (!strcmp(a, "--bw")) bw_kbit = atof(v);
		else if (!strcmp(a, "--rtt")) rtt_ms = atof(v);
		else if (!strcmp(a, "--buffer")) link_buffer = atof(v);
		else if (!strcmp(a, "--timeout")) timeout_s = atof(v);
		else if (!strcmp(a, "--bw-at") && parse_time_arg(v, &t, &rest) &&
		    nbw < MAX_EVENTS) {
			bw_events[nbw].t = t;
			bw_events[nbw].kbit = atof(rest);
			nbw++;
		} else if ((!strcmp(a, "--key") || !strcmp(a, "--command")) &&
		    parse_time_arg(v, &t, &rest) && nkeys < MAX_EVENTS) {
			key_events[nkeys].t = t;
			snprintf(key_events[nkeys].name, sizeof(key_events[nkeys].name),
			    "%s", rest);
			nkeys++;
		} else if (!strcmp(a, "--signal-at") && parse_time_arg(v, &t, &rest)) {
			signal_at = t;
			signal_number = atoi(rest);
		} else if (!strcmp(a, "--resize-at") && parse_time_arg(v, &t, &rest) &&
		    nresize < MAX_EVENTS) {
			resize_events[nresize].t = t;
			if (sscanf(rest, "%dx%d", &resize_events[nresize].cols,
			    &resize_events[nresize].rows) == 2)
				nresize++;
		} else {
			fprintf(stderr, "fake_video_term: bad option %s %s\n", a, v);
			return 2;
		}
	}
	if (i >= argc) {
		fprintf(stderr, "usage: fake_video_term [options] -- command...\n");
		return 2;
	}
	if (!no_video)
		graphics_set_video_backend(&backend);

	if (pipe_mode) {
		int in[2], out[2];
		if (pipe(in) < 0 || pipe(out) < 0) {
			perror("pipe");
			return 1;
		}
		t0 = now_s();
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			int err = open("/dev/null", O_WRONLY);
			setsid();
			dup2(in[0], 0);
			dup2(out[1], 1);
			if (getenv("FAKE_TERM_STDERR"))
				err = open(getenv("FAKE_TERM_STDERR"), O_WRONLY |
				    O_CREAT | O_APPEND, 0644);
			dup2(err, 2);
			close(in[0]); close(in[1]); close(out[0]); close(out[1]);
			unsetenv("SSH_CONNECTION");
			unsetenv("SSH_TTY");
			execvp(argv[i], argv + i);
			_exit(127);
		}
		close(in[0]);
		close(out[1]);
		rfd = out[0];
		wfd = in[1];
		signal(SIGPIPE, SIG_IGN);
		master = -1;
		goto started;
	}
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
	    !(slave_name = ptsname(master))) {
		perror("pty");
		return 1;
	}
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	ws.ws_xpixel = (unsigned short)(cols * cw);
	ws.ws_ypixel = (unsigned short)(rows * ch);
	ioctl(master, TIOCSWINSZ, &ws);
	t0 = now_s();
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		int slave, err;
		setsid();
		slave = open(slave_name, O_RDWR);
		if (slave < 0)
			_exit(127);
#ifdef TIOCSCTTY
		ioctl(slave, TIOCSCTTY, 0);
#endif
		dup2(slave, 0);
		dup2(slave, 1);
		err = open("/dev/null", O_WRONLY);
		if (getenv("FAKE_TERM_STDERR"))
			err = open(getenv("FAKE_TERM_STDERR"), O_WRONLY | O_CREAT |
			    O_APPEND, 0644);
		dup2(err, 2);
		if (slave > 2)
			close(slave);
		close(master);
		/* A slow link stands for SSH; say so, as sshd would. */
		if (bw_kbit > 0 || rtt_ms > 0 || nbw)
			setenv("SSH_CONNECTION", "192.0.2.1 50000 192.0.2.2 22", 1);
		else
			unsetenv("SSH_CONNECTION");
		unsetenv("SSH_TTY");
		execvp(argv[i], argv + i);
		_exit(127);
	}
	rfd = wfd = master;
started:
	fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL) | O_NONBLOCK);
	fcntl(wfd, F_SETFL, fcntl(wfd, F_GETFL) | O_NONBLOCK);

	for (;;) {
		double now = now_s(), t = now - t0, next = now + 0.01, owd = rtt_ms / 2000;
		double kbit = current_bw(t);
		struct pollfd fd = { rfd, 0, 0 };
		int k;
		Chunk *c;

		/* Timed events */
		for (k = 0; k < nkeys; k++)
			if (!key_events[k].done && key_events[k].t <= t) {
				key_events[k].done = 1;
				press(k);
			}
		for (k = 0; k < nresize; k++)
			if (!resize_events[k].done && resize_events[k].t <= t) {
				resize_events[k].done = 1;
				cols = resize_events[k].cols;
				rows = resize_events[k].rows;
				ws.ws_col = (unsigned short)cols;
				ws.ws_row = (unsigned short)rows;
				ws.ws_xpixel = (unsigned short)(cols * cw);
				ws.ws_ypixel = (unsigned short)(rows * ch);
				if (master >= 0)
					ioctl(master, TIOCSWINSZ, &ws);
			}
		if (!exited && signal_at >= 0 && t >= signal_at) {
			kill(pid, signal_number);
			signal_at = -1;
		}
		if (!exited && t > timeout_s && killed_at < 0) {
			timed_out = 1;
			killed_at = t;
			kill(-pid, SIGTERM);
		}
		if (!exited && killed_at >= 0 && t > killed_at + 2)
			kill(-pid, SIGKILL);

		/* Deliver downstream bytes that arrived. */
		while (down.head && down.head->at <= now) {
			c = fifo_pop(&down);
			terminal_feed(c->data, c->length);
			wire_bytes += c->length;
			sec_wire += c->length;
			free(c);
		}
		/* Upstream: replies and keys.  Like st, never drop them: what
		 * the program hasn't read waits (and is counted). */
		while (up.head && up.head->at <= now) {
			ssize_t w;
			c = up.head;
			if (exited) {
				stray_after_exit += c->length;
				free(fifo_pop(&up));
				continue;
			}
			w = write(wfd, c->data, c->length);
			if (w < 0) {
				reply_blocked++;
				break;
			}
			if ((size_t)w < c->length) {
				memmove(c->data, c->data + w, c->length - (size_t)w);
				c->length -= (size_t)w;
				up.bytes -= (size_t)w;
				reply_blocked++;
				break;
			}
			free(fifo_pop(&up));
		}
		if (the_player)
			player_update(the_player);

		/* Per-second trace and steady-state windows. */
		if (t - last_sec >= 1.0) {
			VideoPlayer *p = the_player;
			int64_t end = p ? available_end(p) : 0;
			double ahead = p && p->clock_set && end != INT64_MIN ?
			    (end - p->clock) / 1e6 : 0;
			if (nwindows < 4096) {
				window_rates[nwindows] = sec_wire * 8 / 1000.0 / (t - last_sec);
				window_video[nwindows] = sec_video * 8 / 1000.0 / (t - last_sec);
				window_states[nwindows] = p ? p->state : -1;
				window_rebuffered[nwindows] = rebuffers;
				nwindows++;
			}
			if (trace)
				printf("trace t=%.0f wire_kbit=%.0f video_kbit=%.0f state=%d "
				    "pos=%.2f ahead=%.2f size=%dx%d rebuffers=%d linkq=%zu\n",
				    t, sec_wire * 8 / 1000.0 / (t - last_sec),
				    sec_video * 8 / 1000.0 / (t - last_sec),
				    p ? p->state : -1, p ? p->clock / 1e6 : 0, ahead,
				    p ? p->width : 0, p ? p->height : 0, rebuffers,
				    down.bytes);
			sec_wire = sec_video = 0;
			last_sec = t;
		}
		(void)last_rate_sample_t;

		/* Read from the program while the link has room. */
		{
			double backlog = kbit > 0 ? (link_free - now) * kbit * 125 : 0;
			if (backlog < 0)
				backlog = 0;
			if (!exited && backlog < link_buffer)
				fd.events = POLLIN;
			if (exited && !down.head && !up.head)
				break;
			if (exited && now - t0 > child_exit + 3)
				break;
		}
		if (down.head && down.head->at < next) next = down.head->at;
		if (up.head && up.head->at < next) next = up.head->at;
		{
			int timeout = (int)((next - now_s()) * 1000);
			if (timeout < 0)
				timeout = 0;
			if (fd.events)
				select_poll(&fd, 1, timeout);
			else if (timeout > 0)
				select_poll(NULL, 0, timeout);
		}
		if (fd.revents & POLLIN) {
			unsigned char buf[65536];
			ssize_t n = read(rfd, buf, sizeof(buf));
			if (n > 0) {
				double nw = now_s(), start, finish;
				kbit = current_bw(nw - t0);
				start = link_free > nw ? link_free : nw;
				finish = kbit > 0 ? start + n * 8 / (kbit * 1000) : nw;
				link_free = finish;
				fifo_push(&down, finish + owd, buf, (size_t)n);
			}
		}
		if (!exited) {
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid) {
				exited = 1;
				child_exit = rel();
				if (master >= 0 && tcgetattr(master, &final_termios) == 0)
					have_final_termios = 1;
				/* Drain what's left in the PTY. */
				for (;;) {
					unsigned char buf[65536];
					ssize_t n = read(rfd, buf, sizeof(buf));
					if (n <= 0)
						break;
					fifo_push(&down, now_s() + owd, buf, (size_t)n);
				}
			}
		}
	}
	if (!exited) {
		kill(-pid, SIGKILL);
		waitpid(pid, &status, 0);
	}

	/* Results */
	printf("startup_open=%.3f\n", first_open);
	printf("startup_playing=%.3f\n", first_playing);
	printf("child_exit=%.3f\n", child_exit);
	printf("exit_status=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 +
	    WTERMSIG(status));
	printf("timed_out=%d\n", timed_out);
	printf("wire_bytes=%llu\n", wire_bytes);
	printf("video_bytes=%llu\n", video_bytes);
	printf("rebuffers=%d\n", rebuffers);
	printf("decode_errors=%d\n", decode_errors);
	printf("key_violations=%d\n", key_violations);
	printf("opens=%d flushes=%d destroys=%d max_videos=%d\n", opens, flushes,
	    destroys, max_videos);
	printf("videos_left=%zu\n", graphics_video_count());
	printf("alt_screen_left_on=%d\n", alt_screen);
	printf("cursor_left_hidden=%d\n", cursor_hidden);
	printf("parser_ground=%d\n", pstate == G_GROUND);
	printf("stray_reply_bytes=%llu\n", stray_after_exit);
	printf("reply_blocked=%d replies=%lu\n", reply_blocked, replies);
	if (have_final_termios)
		printf("termios_cooked=%d\n", (final_termios.c_lflag & ICANON) &&
		    (final_termios.c_lflag & ECHO) ? 1 : 0);
	printf("sizes=");
	for (i = 0; i < nsizes; i++)
		printf("%s%.1f:%dx%d", i ? "," : "", sizes[i].t, sizes[i].w,
		    sizes[i].h);
	printf("\n");
	printf("wire_kbit_per_s=");
	for (i = 0; i < nwindows; i++)
		printf("%s%.0f", i ? "," : "", window_rates[i]);
	printf("\n");
	printf("video_kbit_per_s=");
	for (i = 0; i < nwindows; i++)
		printf("%s%.0f", i ? "," : "", window_video[i]);
	printf("\n");
	for (i = 0; i < nkey_metrics; i++) {
		KeyMetric *m = &key_metrics[i];
		printf("key %s at=%.2f effect_latency=%.3f", m->name, m->pressed,
		    m->effect >= 0 ? m->effect - m->pressed : -1);
		if (m->playing >= 0)
			printf(" playing_latency=%.3f", m->playing - m->pressed);
		printf("\n");
	}
	(void)window_states;
	(void)window_rebuffered;
	return timed_out ? 3 : 0;
}

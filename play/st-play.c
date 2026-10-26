/* st-play: a terminal video player for st (docs/video.md).
 *
 * Transcodes any input libavformat opens to H.264 + AAC and streams it to
 * the terminal it runs in through Kitty graphics APCs (a=v), so it plays
 * over SSH.  The terminal decodes and plays; st-play adapts bitrate and
 * resolution to the measured delivery rate and keeps the bytes in flight
 * small so keys act at once.  See play/flow.h for the flow control. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "flow.h"
#include "input.h"
#include "media.h"
#include "out.h"

#define TAG_STATUS 1
#define STATE_UNKNOWN (-1)
enum { ST_BUFFERING, ST_PLAYING, ST_PAUSED, ST_ENDED };

typedef struct {
	/* Options */
	const char *input;
	int inline_rows;
	int copy;
	double max_kbps, fixed_kbps;
	const char *preset;
	double start;
	double volume;
	int muted;
	int prebuffer_ms;
	int no_audio;
	double ahead_cap;
	FILE *stats;

	/* Terminal */
	int tty;
	struct termios saved;
	int raw;
	int cols, rows, cw, ch;
	int cell_queried;
	uint32_t id;
	InputParser in;
	double esc_time;
	OutQueue out;
	Flow flow;
	Adapt adapt;
	int local;

	/* Media */
	Media *media;
	MediaInfo info;
	int media_done, media_wait, media_failed;
	double fps;
	int copying;
	double copy_bad_since, copy_good_since;
	/* Encoder speed: media seconds produced per second spent producing. */
	int preset_shift;
	double busy, busy_media, speed_since, last_shift;

	/* Geometry */
	int vw, vh;		/* display size of the video (terminal pixels) */
	int vcol, vrow, vx, vy;	/* placement, relative to the area */
	int area_rows;
	int opened;

	/* Playback */
	int paused;
	int64_t seg_start;
	int64_t sent_video, sent_audio;
	int eof_sent;
	int state;
	int64_t pos;
	double pos_time;
	int pos_known;
	int64_t vbuf, abuf;
	uint32_t status_seq;
	long long shown, dropped;
	char term_error[48];
	uint32_t valid_seq;
	int64_t last_seek_target;
	double last_seek_time;
	double resize_at;
	double next_status, next_adapt, next_stats;
	int quit;
	int exit_code;
	char message[512];
	double start_time;
	unsigned long long media_bytes;
} App;

static App app;
static int signal_pipe[2] = { -1, -1 };
static volatile sig_atomic_t got_quit, got_winch, got_hup;
static int emergency_tty = -1;
static struct termios emergency_termios;
static int emergency_have_termios;
static char emergency_text[128];
static size_t emergency_length;

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Terminal setup and restore */

static void
on_signal(int sig)
{
	int saved = errno;
	char c = (char)sig;
	ssize_t r;

	if (sig == SIGWINCH)
		got_winch = 1;
	else if (sig == SIGHUP)
		got_hup = got_quit = 1;
	else
		got_quit = 1;
	r = write(signal_pipe[1], &c, 1);
	(void)r;
	errno = saved;
}

static void
on_fatal(int sig)
{
	ssize_t r;
	if (emergency_tty >= 0) {
		r = write(emergency_tty, emergency_text, emergency_length);
		(void)r;
		if (emergency_have_termios)
			tcsetattr(emergency_tty, TCSANOW, &emergency_termios);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
install_signals(void)
{
	struct sigaction sa;
	int fatal[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
	size_t i;

	if (pipe(signal_pipe) == 0) {
		fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
		fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK);
		fcntl(signal_pipe[0], F_SETFD, FD_CLOEXEC);
		fcntl(signal_pipe[1], F_SETFD, FD_CLOEXEC);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTTOU, &sa, NULL);
	sa.sa_handler = on_fatal;
	sa.sa_flags = SA_RESETHAND;
	for (i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
		sigaction(fatal[i], &sa, NULL);
}

static int
set_raw(App *a)
{
	struct termios t;

	if (tcgetattr(a->tty, &a->saved) < 0)
		return -1;
	emergency_termios = a->saved;
	emergency_have_termios = 1;
	t = a->saved;
	t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
	    IGNCR | ICRNL | IXON);
	t.c_oflag &= ~(tcflag_t)OPOST;
	t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(a->tty, TCSANOW, &t) < 0)
		return -1;
	a->raw = 1;
	return 0;
}

static void
restore_tty(App *a)
{
	if (a->raw) {
		tcsetattr(a->tty, TCSAFLUSH, &a->saved);
		a->raw = 0;
	}
	emergency_tty = -1;
}

/* Blocking-with-deadline write, for setup and teardown only. */
static int
write_all(App *a, const char *data, size_t length, double deadline)
{
	while (length) {
		ssize_t n = write(a->tty, data, length);
		if (n > 0) {
			data += n;
			length -= (size_t)n;
			continue;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			return -1;
		{
			struct pollfd p = { a->tty, POLLOUT, 0 };
			double left = deadline - now_s();
			if (left <= 0)
				return -1;
			poll(&p, 1, (int)(left * 1000) + 1);
		}
	}
	return 0;
}

static void
get_winsize(App *a)
{
	struct winsize ws;

	if (ioctl(a->tty, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
		a->cols = ws.ws_col;
		a->rows = ws.ws_row;
		if (!a->cell_queried && ws.ws_xpixel && ws.ws_ypixel) {
			a->cw = ws.ws_xpixel / ws.ws_col;
			a->ch = ws.ws_ypixel / ws.ws_row;
		}
	}
	if (a->cols <= 0) a->cols = 80;
	if (a->rows <= 0) a->rows = 24;
}

/* Output */

static void
queue_apc(App *a, int kind, int tag, const char *controls, const char *text)
{
	size_t tl = text ? strlen(text) : 0;
	OutMsg *m = out_msg_new(kind, apc_length(strlen(controls), tl, OUT_CHUNK));
	if (!m)
		return;
	m->tag = tag;
	m->length = apc_build((char *)m->data, controls, NULL, 0,
	    (const unsigned char *)text, tl, OUT_CHUNK);
	out_push(&a->out, m);
}

static void
queue_text(App *a, int tag, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	int n;
	OutMsg *m;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof(buf))
		n = sizeof(buf) - 1;
	if (tag)
		out_drop_tag(&a->out, tag);
	m = out_msg_new(OUT_CONTROL, (size_t)n);
	if (!m)
		return;
	m->tag = tag;
	memcpy(m->data, buf, (size_t)n);
	out_push(&a->out, m);
}

static void
video_command(App *a, const char *k, int quiet, const char *fmt, ...)
{
	char controls[64], text[1024];
	va_list ap;

	snprintf(controls, sizeof(controls), "a=v,i=%u,k=%s,q=%d", a->id, k, quiet);
	text[0] = '\0';
	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(text, sizeof(text), fmt, ap);
		va_end(ap);
	}
	queue_apc(a, OUT_CONTROL, 0, controls, text[0] ? text : NULL);
}

static void
queue_ping(App *a)
{
	char controls[48], text[32];
	OutMsg *m;
	uint32_t seq = flow_next_seq(&a->flow);

	snprintf(controls, sizeof(controls), "a=v,i=%u,k=q", a->id);
	snprintf(text, sizeof(text), "seq=%u", seq);
	m = out_msg_new(OUT_PING, apc_length(strlen(controls), strlen(text),
	    OUT_CHUNK));
	if (!m)
		return;
	m->seq = seq;
	m->length = apc_build((char *)m->data, controls, NULL, 0,
	    (const unsigned char *)text, strlen(text), OUT_CHUNK);
	out_push(&a->out, m);
}

static void
emit_packet(void *context, int stream, int keyframe, int64_t pts,
		const uint8_t *data, size_t length)
{
	App *a = context;
	char controls[48];
	unsigned char header[10];
	size_t cl;
	OutMsg *m;

	snprintf(controls, sizeof(controls), "a=v,i=%u,k=d,q=2", a->id);
	cl = strlen(controls);
	m = out_msg_new(OUT_MEDIA, apc_length(cl, length + 10, OUT_CHUNK));
	if (!m)
		return;
	packet_header(header, stream, keyframe, pts);
	m->length = apc_build((char *)m->data, controls, header, 10, data,
	    length, OUT_CHUNK);
	m->pts = pts;
	m->stream = stream;
	out_push(&a->out, m);
	a->media_bytes += length;
	if (stream == 0) {
		if (a->sent_video == MEDIA_TIME_NONE || pts > a->sent_video)
			a->sent_video = pts;
	} else {
		int64_t end = pts + 1024 * 1000000LL / MEDIA_AUDIO_RATE;
		if (a->sent_audio == MEDIA_TIME_NONE || end > a->sent_audio)
			a->sent_audio = end;
	}
}

/* Writes queued output while the window allows; never blocks. */
static void
pump_output(App *a, double now)
{
	while (a->out.head) {
		OutMsg *m = a->out.head;
		ssize_t n;

		if (!m->offset) {
			if (m->kind == OUT_MEDIA) {
				if (flow_ping_due(&a->flow, now, 1)) {
					queue_ping(a);
					continue;
				}
				if (!flow_may_send(&a->flow))
					return;
			} else if (m->kind == OUT_PING && !m->stream) {
				flow_ping_written(&a->flow, m->seq, now);
				m->stream = 1;
			}
		}
		n = write(a->tty, m->data + m->offset, m->length - m->offset);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
			got_hup = got_quit = 1;
			return;
		}
		flow_wrote(&a->flow, (size_t)n);
		out_advance(&a->out, (size_t)n);
	}
}

/* Position and buffer */

static int64_t
position_estimate(App *a, double now)
{
	int64_t pos;

	if (!a->pos_known)
		return a->seg_start;
	pos = a->pos;
	if (a->state == ST_PLAYING) {
		double elapsed = now - a->pos_time + a->flow.srtt / 2;
		pos += (int64_t)(elapsed * 1e6);
	}
	return pos;
}

static int64_t
sent_end(App *a)
{
	if (a->info.has_video)
		return a->sent_video;
	return a->sent_audio;
}

static double
media_ahead(App *a, double now)
{
	int64_t end = sent_end(a), pos;

	if (end == MEDIA_TIME_NONE)
		return 0;
	pos = position_estimate(a, now);
	if (pos < a->seg_start)
		pos = a->seg_start;
	return (double)(end - pos) / 1e6;
}

/* Media buffered at the terminal (its newest packets minus the position),
 * as of the last status; falls back to what was sent. */
static double
terminal_ahead(App *a, double now)
{
	int64_t end = INT64_MAX, pos;

	if (!a->pos_known || a->status_seq < a->valid_seq)
		return media_ahead(a, now);
	if (a->info.has_video) {
		if (a->vbuf < 0)
			return 0;
		end = a->vbuf;
	}
	if (a->info.has_audio && a->abuf >= 0 && a->abuf < end)
		end = a->abuf;
	if (end == INT64_MAX)
		return media_ahead(a, now);
	pos = position_estimate(a, now);
	return (double)(end - pos) / 1e6;
}

static double
ahead_cap(App *a, double now)
{
	return a->ahead_cap + (adapt_probing(&a->adapt, now) ? 1.0 : 0);
}

static int
can_produce(App *a, double now)
{
	double limit = flow_window(&a->flow);

	if (!a->media || a->media_done || a->media_failed || !a->opened)
		return 0;
	if (limit < 64 * 1024)
		limit = 64 * 1024;
	if ((double)a->out.media_bytes >= limit)
		return 0;
	return media_ahead(a, now) < ahead_cap(a, now);
}

/* Layout */

static void
layout(App *a)
{
	int area_w, area_h, fw, fh, ox, oy;
	double aspect;

	a->area_rows = a->inline_rows ? a->inline_rows - 1 : a->rows - 1;
	if (a->area_rows < 1)
		a->area_rows = 1;
	area_w = a->cols * a->cw;
	area_h = a->area_rows * a->ch;
	if (!a->info.has_video) {
		a->vw = a->vh = 0;
		return;
	}
	aspect = (double)a->info.width / a->info.height;
	fw = area_w;
	fh = (int)lround(fw / aspect);
	if (fh > area_h) {
		fh = area_h;
		fw = (int)lround(fh * aspect);
	}
	if (fw > 4096) {
		fw = 4096;
		fh = (int)lround(fw / aspect);
	}
	if (fh > 4096) {
		fh = 4096;
		fw = (int)lround(fh * aspect);
	}
	if (fw < 1) fw = 1;
	if (fh < 1) fh = 1;
	ox = (area_w - fw) / 2;
	oy = (area_h - fh) / 2;
	a->vw = fw;
	a->vh = fh;
	a->vcol = ox / a->cw;
	a->vx = ox % a->cw;
	a->vrow = oy / a->ch;
	a->vy = oy % a->ch;
}

/* Move to row (0-based, relative to the area) and column. */
static void
queue_goto(App *a, int tag, int row, int col)
{
	if (a->inline_rows) {
		/* ESC 8 restores the top-left saved when the area was made. */
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "\0338");
		if (row > 0)
			n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\033[%dB", row);
		if (col > 0)
			n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\033[%dC", col);
		queue_text(a, tag, "%s", buf);
	} else {
		queue_text(a, tag, "\033[%d;%dH", row + 1, col + 1);
	}
}

static void
apply_encoder(App *a)
{
	const Rung *r = &a->adapt.rungs[a->adapt.rung];
	const char *preset = a->preset ? a->preset : media_preset_for(
	    (double)r->width * r->height * a->fps, a->preset_shift);
	media_set_video(a->media, r->width, r->height, a->fps,
	    (int)a->adapt.kbps, preset);
}

/* Step to a faster x264 preset when the encoder can't stay well ahead of
 * real time (a slow or busy machine), back once there is room. */
static void
speed_tick(App *a, double now)
{
	double speed;

	if (a->preset || a->copying || now - a->speed_since < 3)
		return;
	if (a->busy > 0.5) {
		speed = a->busy_media / a->busy;
		if (speed < 1.5 && a->preset_shift < 3 && now - a->last_shift > 4) {
			a->preset_shift++;
			a->last_shift = now;
			apply_encoder(a);
		} else if (speed > 6 && a->preset_shift > 0 &&
		    now - a->last_shift > 30) {
			a->preset_shift--;
			a->last_shift = now;
			apply_encoder(a);
		}
	}
	a->busy = a->busy_media = 0;
	a->speed_since = now;
}

static double
choose_audio_kbps(App *a)
{
	double rate = a->flow.rate;
	if (a->local || rate > 1.5e6)
		return 160;
	if (rate > 400e3)
		return 128;
	if (rate > 150e3)
		return 96;
	return 64;
}

static void
setup_adapt(App *a, double now)
{
	Rung rungs[ADAPT_RUNGS];
	int count, mw, mh;
	double audio = a->info.has_audio ? choose_audio_kbps(a) : 0;

	media_set_audio_kbps(a->media, (int)audio);
	if (!a->info.has_video)
		return;
	/* Never upscale beyond the source or the placement. */
	mw = a->info.width;
	mh = a->info.height;
	if (a->vw < mw || a->vh < mh) {
		mw = a->vw;
		mh = a->vh;
	}
	count = adapt_ladder(rungs, ADAPT_RUNGS, mw, mh);
	adapt_init(&a->adapt, now, rungs, count, a->fps, a->flow.rate, audio,
	    a->max_kbps);
	a->adapt.ahead_target = 0.6 * a->ahead_cap;
	if (a->fixed_kbps > 0) {
		a->adapt.fixed = 1;
		a->adapt.kbps = a->fixed_kbps;
		a->adapt.rung = count - 1;
		/* Pick the rung the fixed bitrate affords. */
		while (a->adapt.rung > 0 && a->fixed_kbps * 1000 /
		    ((double)rungs[a->adapt.rung].width *
		    rungs[a->adapt.rung].height * a->fps) < ADAPT_BPP_DOWN)
			a->adapt.rung--;
	}
	apply_encoder(a);
}

static void
open_video(App *a)
{
	char config[512];
	int n = 0;

	if (a->info.has_video)
		n += snprintf(config + n, sizeof(config) - (size_t)n,
		    "video=h264 width=%d height=%d ", a->vw, a->vh);
	if (a->info.has_audio)
		n += snprintf(config + n, sizeof(config) - (size_t)n,
		    "audio=aac rate=%d channels=2 ", MEDIA_AUDIO_RATE);
	snprintf(config + n, sizeof(config) - (size_t)n,
	    "prebuffer=%d volume=%.3f pause=%d", a->prebuffer_ms,
	    a->muted ? 0.0 : a->volume, a->paused);
	video_command(a, "o", 1, "%s", config);
	if (a->info.has_video) {
		char controls[128];
		queue_goto(a, 0, a->vrow, a->vcol);
		snprintf(controls, sizeof(controls),
		    "a=p,i=%u,p=1,X=%d,Y=%d,z=-1,C=1,q=1", a->id, a->vx, a->vy);
		queue_apc(a, OUT_CONTROL, 0, controls, NULL);
	}
	a->opened = 1;
}

static void
clear_area(App *a)
{
	if (a->inline_rows)
		queue_text(a, 0, "\0338\033[J\0338");
	else
		queue_text(a, 0, "\033[H\033[2J");
}

/* Restart the stream at `target`: a flush (or a reopen) and an IDR. */
static void
restart_stream(App *a, int64_t target, int reopen, double now)
{
	if (a->info.duration != MEDIA_TIME_NONE && target > a->info.duration - 500000)
		target = a->info.duration - 500000;
	if (target < 0)
		target = 0;
	out_drop_media(&a->out);
	if (reopen)
		open_video(a);
	else
		video_command(a, "f", 1, "start=%lld", (long long)target);
	media_seek(a->media, target);
	media_set_audio_kbps(a->media, a->info.has_audio ?
	    (int)choose_audio_kbps(a) : 0);
	a->media_done = a->media_failed = a->eof_sent = 0;
	a->seg_start = target;
	a->sent_video = a->sent_audio = MEDIA_TIME_NONE;
	a->valid_seq = a->flow.next_seq;
	a->pos = target;
	a->pos_known = 0;
	a->state = ST_BUFFERING;
	a->last_seek_target = target;
	a->last_seek_time = now;
	a->term_error[0] = '\0';
}

static void
seek_by(App *a, double delta, double now)
{
	int64_t base;

	if (!a->info.seekable)
		return;
	/* Repeated presses before the terminal caught up add up. */
	if (!a->pos_known && now - a->last_seek_time < 3)
		base = a->last_seek_target;
	else
		base = position_estimate(a, now);
	restart_stream(a, base + (int64_t)(delta * 1e6), 0, now);
}

static void
relayout(App *a, double now)
{
	int ow = a->vw, oh = a->vh;

	get_winsize(a);
	if (a->inline_rows && a->inline_rows > a->rows)
		a->inline_rows = a->rows;
	layout(a);
	clear_area(a);
	if (a->info.has_video && (a->vw != ow || a->vh != oh)) {
		int64_t pos = position_estimate(a, now);
		setup_adapt(a, now);
		restart_stream(a, pos, 1, now);
	} else if (a->info.has_video) {
		char controls[128];
		queue_goto(a, 0, a->vrow, a->vcol);
		snprintf(controls, sizeof(controls),
		    "a=p,i=%u,p=1,X=%d,Y=%d,z=-1,C=1,q=1", a->id, a->vx, a->vy);
		queue_apc(a, OUT_CONTROL, 0, controls, NULL);
	}
	a->next_status = 0;
}

/* Status line */

static void
format_time(char *out, size_t size, int64_t us)
{
	long long s = us < 0 ? 0 : us / 1000000;
	if (s >= 3600)
		snprintf(out, size, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, size, "%lld:%02lld", s / 60, s % 60);
}

static void
draw_status(App *a, double now)
{
	char line[1024], left[160], right[200], cur[32], dur[32];
	int64_t pos = position_estimate(a, now);
	const char *icon;
	int width = a->cols, bar, filled, n, i, used;
	double ahead = media_ahead(a, now);

	if (a->info.duration != MEDIA_TIME_NONE && pos > a->info.duration)
		pos = a->info.duration;
	format_time(cur, sizeof(cur), pos);
	format_time(dur, sizeof(dur), a->info.duration);
	if (a->paused)
		icon = "\xe2\x8f\xb8";	/* ⏸ */
	else if (a->state == ST_BUFFERING || a->state == STATE_UNKNOWN)
		icon = "\xe2\x80\xa6";	/* … */
	else
		icon = "\xe2\x96\xb6";	/* ▶ */
	snprintf(left, sizeof(left), " %s %s / %s ", icon, cur,
	    a->info.duration != MEDIA_TIME_NONE ? dur : "--:--");
	{
		char video[64] = "", vol[32];
		if (a->info.has_video) {
			int w, h;
			media_encoder_size(a->media, &w, &h);
			if (a->copying)
				snprintf(video, sizeof(video), "copy %dx%d ", w, h);
			else
				snprintf(video, sizeof(video), "%dx%d %.1f Mb/s ",
				    w, h, a->adapt.kbps / 1000);
		}
		if (a->muted)
			snprintf(vol, sizeof(vol), "muted");
		else
			snprintf(vol, sizeof(vol), "vol %d%%",
			    (int)lround(a->volume * 100));
		snprintf(right, sizeof(right), " %s%s%sbuf %.1fs %s ",
		    a->state == ST_BUFFERING && !a->paused ? "buffering " : "",
		    a->term_error[0] ? a->term_error : "",
		    a->term_error[0] ? " " : "", ahead < 0 ? 0 : ahead, vol);
		{
			char tmp[200];
			snprintf(tmp, sizeof(tmp), " %s%s", video, right + 1);
			snprintf(right, sizeof(right), "%s", tmp);
		}
	}
	/* Display widths: the icon is one column, the rest ASCII. */
	used = (int)strlen(left) - 2 + (int)strlen(right);
	bar = width - used;
	if (bar < 4) {
		/* Narrow: drop the details. */
		snprintf(right, sizeof(right), " ");
		used = (int)strlen(left) - 2 + 1;
		bar = width - used;
	}
	n = snprintf(line, sizeof(line), "%s", left);
	if (bar >= 4) {
		double frac = a->info.duration > 0 && a->info.duration !=
		    MEDIA_TIME_NONE ? (double)pos / a->info.duration : 0;
		if (frac < 0) frac = 0;
		if (frac > 1) frac = 1;
		filled = (int)lround(frac * bar);
		for (i = 0; i < bar && n < (int)sizeof(line) - 8; i++)
			n += snprintf(line + n, sizeof(line) - (size_t)n, "%s",
			    i < filled ? "\xe2\x94\x81" : "\xe2\x94\x80");
	}
	snprintf(line + n, sizeof(line) - (size_t)n, "%s", right);
	if (a->inline_rows) {
		char buf[64];
		int k = snprintf(buf, sizeof(buf), "\0338");
		if (a->inline_rows > 1)
			snprintf(buf + k, sizeof(buf) - (size_t)k, "\033[%dB",
			    a->inline_rows - 1);
		queue_text(a, TAG_STATUS, "%s\r\033[2K%s", buf, line);
	} else {
		queue_text(a, TAG_STATUS, "\033[%d;1H\033[2K%s", a->rows, line);
	}
}

/* Input */

static void
set_volume(App *a)
{
	video_command(a, "s", 1, "volume=%.3f", a->muted ? 0.0 : a->volume);
}

static void
handle_key(App *a, int key, double now)
{
	switch (key) {
	case 'q': case 'Q': case 3: case KEY_ESCAPE: case 4:
		a->quit = 1;
		break;
	case ' ': case 'p': case 'P':
		a->paused = !a->paused;
		video_command(a, "s", 1, "pause=%d", a->paused);
		if (a->pos_known && a->state == ST_PLAYING && a->paused) {
			a->pos = position_estimate(a, now);
			a->pos_time = now;
		}
		break;
	case KEY_LEFT: seek_by(a, -5, now); break;
	case KEY_RIGHT: seek_by(a, 5, now); break;
	case KEY_DOWN: seek_by(a, -60, now); break;
	case KEY_UP: seek_by(a, 60, now); break;
	case KEY_PAGE_DOWN: seek_by(a, -600, now); break;
	case KEY_PAGE_UP: seek_by(a, 600, now); break;
	case KEY_HOME:
		if (a->info.seekable)
			restart_stream(a, 0, 0, now);
		break;
	case 'm': case 'M':
		a->muted = !a->muted;
		set_volume(a);
		break;
	case '9': case '/':
		a->volume = a->volume - 0.1 < 0 ? 0 : a->volume - 0.1;
		a->muted = 0;
		set_volume(a);
		break;
	case '0': case '*':
		a->volume = a->volume + 0.1 > 1 ? 1 : a->volume + 0.1;
		a->muted = 0;
		set_volume(a);
		break;
	case 12:	/* Ctrl-L */
		a->resize_at = now;
		break;
	}
	a->next_status = 0;
}

static void
handle_reply(App *a, const Reply *r, double now)
{
	if (r->id != a->id)
		return;
	if (r->has_seq) {
		flow_ack(&a->flow, (uint32_t)r->seq, now);
		if (r->seq < a->valid_seq)
			return;	/* about the stream before the last seek */
		if (r->state >= 0)
			a->state = r->state;
		if (r->pos >= 0) {
			a->pos = r->pos;
			a->pos_time = now;
			a->pos_known = 1;
		}
		a->vbuf = r->vbuf;
		a->abuf = r->abuf;
		a->status_seq = (uint32_t)r->seq;
		a->shown = r->shown;
		a->dropped = r->dropped;
		snprintf(a->term_error, sizeof(a->term_error), "%s", r->error);
		if (a->state == ST_ENDED && a->eof_sent) {
			a->quit = 1;
		}
		return;
	}
	if (!r->ok) {
		/* An open or placement failed (ENOSPC, EINVAL, ...). */
		size_t n = r->message_length < 200 ? r->message_length : 200;
		snprintf(a->message, sizeof(a->message), "terminal: %.*s",
		    (int)n, r->message);
		if (!strncmp(r->message, "ENOSPC", 6) ||
		    !strncmp(r->message, "ENOSYS", 6) ||
		    !strncmp(r->message, "EIO", 3) ||
		    !strncmp(r->message, "ENOENT", 6)) {
			a->exit_code = 1;
			a->quit = 1;
		}
	}
}

static void
input_event(const InputEvent *e, void *context)
{
	App *a = context;
	double now = now_s();

	if (e->type == INPUT_KEY) {
		handle_key(a, e->key, now);
	} else if (e->type == INPUT_APC) {
		Reply r;
		if (reply_parse(e->text, e->length, &r))
			handle_reply(a, &r, now);
	} else if (e->type == INPUT_CELL_SIZE && e->width > 0 && e->height > 0) {
		if (a->cw != e->width || a->ch != e->height) {
			a->cw = e->width;
			a->ch = e->height;
			a->cell_queried = 1;
			if (a->opened)
				a->resize_at = now;
		}
	}
}

static void
read_input(App *a)
{
	unsigned char buf[4096];
	ssize_t n;

	while ((n = read(a->tty, buf, sizeof(buf))) > 0) {
		input_feed(&a->in, buf, (size_t)n, input_event, a);
		if (input_pending_escape(&a->in))
			a->esc_time = now_s();
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		got_hup = got_quit = 1;
}

/* Startup: capability query */

typedef struct {
	App *app;
	int got_caps, got_da1, caps_ok, cell;
	double caps_time, da1_time;
	char caps[256];
} Probe;

static void
probe_event(const InputEvent *e, void *context)
{
	Probe *p = context;
	if (e->type == INPUT_APC) {
		Reply r;
		if (reply_parse(e->text, e->length, &r) && r.id == p->app->id &&
		    !p->got_caps) {
			p->got_caps = 1;
			p->caps_time = now_s();
			p->caps_ok = r.ok && r.video_h264 && r.audio_aac;
			snprintf(p->caps, sizeof(p->caps), "%.*s",
			    (int)(r.message_length < 200 ? r.message_length : 200),
			    r.message);
		}
	} else if (e->type == INPUT_DA1) {
		p->got_da1 = 1;
		p->da1_time = now_s();
	} else if (e->type == INPUT_CELL_SIZE && e->width > 0 && e->height > 0) {
		p->app->cw = e->width;
		p->app->ch = e->height;
		p->app->cell_queried = 1;
	}
}

static void
probe_wait(App *a, Probe *p, double deadline)
{
	while (!p->got_da1 && !got_quit) {
		struct pollfd fd = { a->tty, POLLIN, 0 };
		double left = deadline - now_s();
		unsigned char buf[4096];
		ssize_t n;

		if (left <= 0)
			break;
		if (poll(&fd, 1, (int)(left * 1000) + 1) <= 0)
			continue;
		while ((n = read(a->tty, buf, sizeof(buf))) > 0)
			input_feed(&a->in, buf, (size_t)n, probe_event, p);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
			break;
	}
}

/* Teardown */

static void
shutdown_terminal(App *a)
{
	char buf[256];
	int n;
	double deadline;
	OutMsg *head = a->out.head;

	if (!a->raw)
		return;
	if (got_hup) {
		restore_tty(a);
		return;
	}
	deadline = now_s() + 2.0;
	/* Never leave the terminal inside a half-written APC. */
	if (head && head->offset && head->offset < head->length) {
		if (head->kind == OUT_MEDIA)
			write_all(a, "\033\\", 2, deadline);
		else
			write_all(a, (char *)head->data + head->offset,
			    head->length - head->offset, deadline);
	}
	out_clear(&a->out);
	n = snprintf(buf, sizeof(buf), "\033_Ga=d,d=I,i=%u,q=2\033\\", a->id);
	if (a->inline_rows)
		n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\0338\033[J");
	else
		n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\033[?1049l");
	n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\033[?25h\033[c");
	if (write_all(a, buf, (size_t)n, deadline) == 0) {
		/* Swallow the replies still on their way (pings), up to the
		 * DA1 answer, so none reach the shell as typed text. */
		Probe p;
		double wait = 4 * a->flow.srtt + 0.3;
		memset(&p, 0, sizeof(p));
		p.app = a;
		input_init(&a->in);
		if (wait > 3)
			wait = 3;
		probe_wait(a, &p, now_s() + wait);
	}
	restore_tty(a);
}

/* Stats */

static void
write_stats(App *a, double now)
{
	int w = 0, h = 0;
	if (!a->stats)
		return;
	if (a->media && a->info.has_video)
		media_encoder_size(a->media, &w, &h);
	fprintf(a->stats,
	    "t=%.2f rate=%.0f window=%.0f inflight=%llu srtt=%.3f min_rtt=%.3f "
	    "ahead=%.2f kbps=%.0f usable=%.0f size=%dx%d state=%d pos=%.2f "
	    "sample=%.0f valid=%d probing=%d queue=%zu copy=%d\n",
	    now - a->start_time, a->flow.rate * 8 / 1000,
	    flow_window(&a->flow) / 1024,
	    (unsigned long long)flow_inflight(&a->flow) / 1024, a->flow.srtt,
	    a->flow.min_rtt, media_ahead(a, now), a->adapt.kbps,
	    a->adapt.usable_kbps, w, h, a->state,
	    position_estimate(a, now) / 1e6, a->flow.last_sample * 8 / 1000,
	    a->flow.last_sample_valid, adapt_probing(&a->adapt, now),
	    a->out.bytes, a->copying);
	fflush(a->stats);
}

/* Main loop pieces */

static void
adapt_tick(App *a, double now)
{
	/* What the terminal holds, not what we queued: a burst of packets
	 * (a resolution switch) can sit in our queue on a slow link. */
	double ahead = terminal_ahead(a, now);
	int app_limited = !a->out.media_count && !can_produce(a, now);
	int old_rung = a->adapt.rung;
	double old_kbps = a->adapt.kbps;

	if (!a->info.has_video)
		return;
	if (a->paused || a->state == ST_PAUSED)
		ahead = a->ahead_cap;
	if (adapt_update(&a->adapt, &a->flow, now, ahead, app_limited) ||
	    old_rung != a->adapt.rung || old_kbps != a->adapt.kbps)
		apply_encoder(a);

	if (a->copy && a->info.copyable) {
		double src = a->info.bitrate > 0 ? a->info.bitrate / 1000.0 : 8000;
		double usable = a->adapt.usable_kbps;
		if (a->copying) {
			if (usable < 1.1 * src || (ahead < 1.0 && a->state != ST_PAUSED &&
			    now - a->start_time > 3)) {
				if (!a->copy_bad_since)
					a->copy_bad_since = now;
				if (now - a->copy_bad_since > 1.5) {
					media_set_copy(a->media, 0);
					a->copying = 0;
					a->copy_bad_since = 0;
				}
			} else {
				a->copy_bad_since = 0;
			}
		} else if (usable > 1.5 * src) {
			if (!a->copy_good_since)
				a->copy_good_since = now;
			if (now - a->copy_good_since > 10) {
				media_set_copy(a->media, 1);
				a->copying = 1;
				a->copy_good_since = 0;
			}
		} else {
			a->copy_good_since = 0;
		}
	}
}

static void
produce(App *a, double now)
{
	double deadline = now + 0.01;

	a->media_wait = 0;
	while (can_produce(a, now)) {
		double t1 = now_s();
		int64_t end = sent_end(a);
		int r = media_step(a->media, emit_packet, a);
		if (r == MEDIA_MORE) {
			a->busy += now_s() - t1;
			if (end != MEDIA_TIME_NONE && sent_end(a) > end)
				a->busy_media += (double)(sent_end(a) - end) / 1e6;
		}
		if (r == MEDIA_WAIT) {
			a->media_wait = 1;
			break;
		}
		if (r == MEDIA_EOF) {
			a->media_done = 1;
			if (!a->eof_sent) {
				video_command(a, "e", 1, NULL);
				a->eof_sent = 1;
			}
			break;
		}
		if (r == MEDIA_ERROR) {
			snprintf(a->message, sizeof(a->message), "%s",
			    media_error(a->media));
			a->media_failed = 1;
			a->exit_code = 1;
			a->quit = 1;
			break;
		}
		now = now_s();
		if (now > deadline)
			break;
	}
}

static void
usage(FILE *f)
{
	fprintf(f,
	    "usage: st-play [options] FILE|URL|-\n"
	    "Play video in st (st video, docs/video.md), also over SSH.\n\n"
	    "  --inline ROWS     play in ROWS rows at the cursor instead of fullscreen\n"
	    "  --start SECONDS   start position\n"
	    "  --copy            pass H.264 through when the link is fast enough\n"
	    "  --bitrate KBPS    fixed video bitrate (no adaptation)\n"
	    "  --max-bitrate KBPS  adaptation ceiling\n"
	    "  --preset NAME     x264 preset (default: by resolution)\n"
	    "  --volume 0..1     initial volume (default 1)\n"
	    "  --mute            start muted\n"
	    "  --no-audio        ignore audio\n"
	    "  --prebuffer MS    terminal prebuffer (default 300, 200 locally)\n"
	    "  --ahead SECONDS   media sent ahead of playback (default 5, 2 locally)\n"
	    "  --stats FILE      append flow/adaptation stats every 250 ms\n"
	    "  --log FILE        append ffmpeg's log\n\n"
	    "keys: space/p pause, left/right 5 s, down/up 60 s, PgDn/PgUp 10 min,\n"
	    "      m mute, 9/0 volume, q/Esc/Ctrl-C quit\n");
}

static int
parse_args(App *a, int argc, char **argv)
{
	int i;
	const char *log = NULL;

	a->volume = 1;
	a->prebuffer_ms = -1;
	a->ahead_cap = -1;
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *val = i + 1 < argc ? argv[i + 1] : NULL;
#define NEEDS() do { if (!val) { fprintf(stderr, "st-play: %s needs a value\n", arg); return -1; } i++; } while (0)
		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(stdout);
			exit(0);
		} else if (!strcmp(arg, "--inline")) {
			NEEDS();
			a->inline_rows = atoi(val);
			if (a->inline_rows < 2) {
				fprintf(stderr, "st-play: --inline needs at least 2 rows\n");
				return -1;
			}
		} else if (!strcmp(arg, "--start")) {
			NEEDS();
			a->start = atof(val);
		} else if (!strcmp(arg, "--copy")) {
			a->copy = 1;
		} else if (!strcmp(arg, "--bitrate")) {
			NEEDS();
			a->fixed_kbps = atof(val);
		} else if (!strcmp(arg, "--max-bitrate")) {
			NEEDS();
			a->max_kbps = atof(val);
		} else if (!strcmp(arg, "--preset")) {
			NEEDS();
			a->preset = val;
		} else if (!strcmp(arg, "--volume")) {
			NEEDS();
			a->volume = atof(val);
			if (a->volume < 0) a->volume = 0;
			if (a->volume > 1) a->volume = 1;
		} else if (!strcmp(arg, "--mute")) {
			a->muted = 1;
		} else if (!strcmp(arg, "--no-audio")) {
			a->no_audio = 1;
		} else if (!strcmp(arg, "--prebuffer")) {
			NEEDS();
			a->prebuffer_ms = atoi(val);
		} else if (!strcmp(arg, "--ahead")) {
			NEEDS();
			a->ahead_cap = atof(val);
		} else if (!strcmp(arg, "--stats")) {
			NEEDS();
			a->stats = fopen(val, "a");
			if (!a->stats) {
				fprintf(stderr, "st-play: %s: %s\n", val, strerror(errno));
				return -1;
			}
		} else if (!strcmp(arg, "--log")) {
			NEEDS();
			log = val;
		} else if (arg[0] == '-' && arg[1]) {
			fprintf(stderr, "st-play: unknown option %s\n", arg);
			return -1;
		} else if (!a->input) {
			a->input = arg;
		} else {
			fprintf(stderr, "st-play: one input only\n");
			return -1;
		}
#undef NEEDS
	}
	if (!a->input) {
		usage(stderr);
		return -1;
	}
	media_log_to(log);
	return 0;
}

int
main(int argc, char **argv)
{
	App *a = &app;
	Probe probe;
	char error[512];
	double t0, opened_at, now;
	char query[128];
	int n;

	memset(a, 0, sizeof(*a));
	a->tty = -1;
	if (parse_args(a, argc, argv) < 0)
		return 2;
	a->start_time = now_s();
	a->id = 0x5700000u | ((uint32_t)getpid() & 0xfffffu);
	a->state = STATE_UNKNOWN;
	a->sent_video = a->sent_audio = MEDIA_TIME_NONE;

	a->tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (a->tty < 0) {
		fprintf(stderr, "st-play: no terminal: %s\n", strerror(errno));
		return 1;
	}
	install_signals();
	if (set_raw(a) < 0) {
		fprintf(stderr, "st-play: cannot set up the terminal: %s\n",
		    strerror(errno));
		return 1;
	}
	fcntl(a->tty, F_SETFL, fcntl(a->tty, F_GETFL) | O_NONBLOCK);
	emergency_tty = a->tty;
	emergency_length = (size_t)snprintf(emergency_text, sizeof(emergency_text),
	    "\033\\\033_Ga=d,d=I,i=%u,q=2\033\\%s\033[?25h", a->id,
	    "\033[?1049l");
	input_init(&a->in);
	get_winsize(a);

	/* Ask about video support while the input opens. */
	n = snprintf(query, sizeof(query), "\033_Ga=v,i=%u,k=c\033\\\033[16t\033[c",
	    a->id);
	t0 = now_s();
	write_all(a, query, (size_t)n, t0 + 2);
	a->media = media_open(a->input, !a->no_audio, &a->info, error,
	    sizeof(error));
	opened_at = now_s();
	memset(&probe, 0, sizeof(probe));
	probe.app = a;
	probe_wait(a, &probe, now_s() + 3.0);
	if (!a->media) {
		restore_tty(a);
		fprintf(stderr, "st-play: %s\n", error);
		return 1;
	}
	if (!probe.got_caps || !probe.caps_ok) {
		restore_tty(a);
		if (got_quit)
			return 130;
		if (probe.got_caps)
			fprintf(stderr, "st-play: the terminal can't play video (%s)\n",
			    probe.caps);
		else if (probe.got_da1)
			fprintf(stderr, "st-play: this terminal has no st video "
			    "support (a=v).\nIt needs st with video; in other "
			    "Kitty-graphics terminals try mpv --vo=kitty.\n");
		else
			fprintf(stderr, "st-play: the terminal did not answer\n");
		return 1;
	}
	/* RTT: exact if the reply came after the input opened; otherwise
	 * time one more DA1. */
	{
		double rtt;
		if (probe.caps_time > opened_at + 0.002) {
			rtt = probe.caps_time - t0;
		} else if (opened_at - t0 < 0.01) {
			rtt = opened_at - t0;
		} else {
			Probe again;
			double t1 = now_s();
			memset(&again, 0, sizeof(again));
			again.app = a;
			write_all(a, "\033[c", 3, t1 + 2);
			probe_wait(a, &again, t1 + 3);
			rtt = again.got_da1 ? again.da1_time - t1 : 0.2;
		}
		a->local = rtt < 0.01 && !getenv("SSH_CONNECTION");
		now = now_s();
		flow_init(&a->flow, now, rtt, a->local ? 40e6 : 250e3);
	}
	if (!a->cw || !a->ch) {
		a->cw = 8;
		a->ch = 16;
	}
	if (a->prebuffer_ms < 0)
		a->prebuffer_ms = a->local ? 200 : 300;
	if (a->ahead_cap < 0)
		a->ahead_cap = a->local ? 2.0 : 5.0;
	a->fps = a->info.fps;
	if (!a->local && a->fps > 31 && a->fixed_kbps <= 0)
		a->fps /= 2;
	if (a->inline_rows && a->inline_rows > a->rows)
		a->inline_rows = a->rows;

	/* Screen */
	if (a->inline_rows) {
		char buf[64];
		int k, i;
		write_all(a, "\033[?25l\r", 7, now + 2);
		for (i = 1; i < a->inline_rows; i++)
			write_all(a, "\n", 1, now + 2);
		k = snprintf(buf, sizeof(buf), "\033[%dA\0337", a->inline_rows - 1);
		write_all(a, buf, (size_t)k, now + 2);
		emergency_length = (size_t)snprintf(emergency_text,
		    sizeof(emergency_text),
		    "\033\\\033_Ga=d,d=I,i=%u,q=2\033\\\0338\033[J\033[?25h", a->id);
	} else {
		write_all(a, "\033[?1049h\033[?25l\033[H\033[2J", 21, now + 2);
	}
	layout(a);
	setup_adapt(a, now);
	if (a->copy && a->info.copyable && (a->local || a->adapt.usable_kbps >
	    1.3 * (a->info.bitrate > 0 ? a->info.bitrate / 1000.0 : 8000))) {
		media_set_copy(a->media, 1);
		a->copying = 1;
	}
	open_video(a);
	a->seg_start = 0;
	if (a->start > 0 && a->info.seekable) {
		media_seek(a->media, (int64_t)(a->start * 1e6));
		a->seg_start = (int64_t)(a->start * 1e6);
		a->last_seek_target = a->seg_start;
	}
	a->pos = a->seg_start;
	a->valid_seq = a->flow.next_seq;

	/* Main loop */
	while (!a->quit && !got_quit) {
		struct pollfd fds[3];
		int nfds = 0, timeout, producing;
		double next;

		now = now_s();
		if (got_winch) {
			got_winch = 0;
			a->resize_at = now + 0.15;
			queue_text(a, 0, "\033[16t");
		}
		if (a->resize_at && now >= a->resize_at) {
			a->resize_at = 0;
			relayout(a, now);
		}
		if (input_pending_escape(&a->in) &&
		    now - a->esc_time >= INPUT_ESC_TIMEOUT)
			input_timeout(&a->in, input_event, a);
		if (now >= a->next_adapt) {
			adapt_tick(a, now);
			speed_tick(a, now);
			a->next_adapt = now + 0.1;
		}
		if (now >= a->next_status) {
			draw_status(a, now);
			a->next_status = now + 0.25;
		}
		if (now >= a->next_stats && a->stats) {
			write_stats(a, now);
			a->next_stats = now + 0.25;
		}
		produce(a, now);
		producing = can_produce(a, now) && !a->media_wait;
		if (flow_ping_due(&a->flow, now, a->out.media_count || producing))
			queue_ping(a);
		pump_output(a, now);
		/* Nothing queued with the window open: whatever the reason (the
		 * media budget, a slow encoder or input), the link is not what
		 * limits us, so the acks of this stretch don't measure it. */
		if (!a->out.media_count && flow_may_send(&a->flow))
			flow_idle(&a->flow);
		if (a->quit || got_quit)
			break;

		fds[nfds].fd = a->tty;
		fds[nfds].events = POLLIN | (a->out.head && (a->out.head->offset ||
		    a->out.head->kind != OUT_MEDIA || flow_may_send(&a->flow)) ?
		    POLLOUT : 0);
		nfds++;
		fds[nfds].fd = signal_pipe[0];
		fds[nfds].events = POLLIN;
		nfds++;
		if (a->media) {
			fds[nfds].fd = media_wake_fd(a->media);
			fds[nfds].events = POLLIN;
			nfds++;
		}
		next = a->next_adapt;
		if (a->next_status < next) next = a->next_status;
		if (a->stats && a->next_stats < next) next = a->next_stats;
		if (a->resize_at && a->resize_at < next) next = a->resize_at;
		if (input_pending_escape(&a->in) &&
		    a->esc_time + INPUT_ESC_TIMEOUT < next)
			next = a->esc_time + INPUT_ESC_TIMEOUT;
		{
			double ping_at = a->flow.last_ping_time + FLOW_PING_SENDING;
			if (ping_at < next)
				next = ping_at;
		}
		now = now_s();
		timeout = producing ? 0 : next <= now ? 0 :
		    (int)((next - now) * 1000) + 1;
		if (poll(fds, (nfds_t)nfds, timeout) < 0 && errno != EINTR)
			break;
		if (fds[1].revents & POLLIN) {
			char buf[64];
			while (read(signal_pipe[0], buf, sizeof(buf)) > 0)
				;
		}
		if (nfds > 2 && (fds[2].revents & POLLIN))
			media_drain_wake(a->media);
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
			read_input(a);
	}

	shutdown_terminal(a);
	if (a->stats) {
		write_stats(a, now_s());
		fclose(a->stats);
	}
	if (a->message[0])
		fprintf(stderr, "st-play: %s\n", a->message);
	/* Not media_close(): a demux thread blocked on a pipe can't be
	 * joined; the process exit ends it. */
	return got_quit && !a->exit_code ? (got_hup ? 129 : 0) : a->exit_code;
}

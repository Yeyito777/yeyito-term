/* st-play unit tests: input/reply parser, APC framing and the write queue,
 * flow control and rate estimation (against a simulated link), and the
 * bitrate/resolution adaptation. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "../play/flow.h"
#include "../play/input.h"
#include "../play/out.h"

/* Input parser */

typedef struct {
	int keys[64];
	int nkeys;
	char apc[8][256];
	int napc;
	int da1;
	int cell_w, cell_h;
	char command[4][64];
	int ncommand;
} Events;

static void
collect(const InputEvent *e, void *context)
{
	Events *ev = context;
	switch (e->type) {
	case INPUT_KEY:
		if (ev->nkeys < 64)
			ev->keys[ev->nkeys++] = e->key;
		break;
	case INPUT_APC:
		if (ev->napc < 8)
			snprintf(ev->apc[ev->napc++], 256, "%.*s", (int)e->length, e->text);
		break;
	case INPUT_DA1:
		ev->da1++;
		break;
	case INPUT_COMMAND:
		if (ev->ncommand < 4)
			snprintf(ev->command[ev->ncommand++], 64, "%.*s",
			    (int)e->length, e->text);
		break;
	case INPUT_CELL_SIZE:
		ev->cell_w = e->width;
		ev->cell_h = e->height;
		break;
	}
}

static void
feed(InputParser *p, Events *ev, const char *s)
{
	input_feed(p, (const unsigned char *)s, strlen(s), collect, ev);
}

TEST(input_keys)
{
	InputParser p;
	Events ev;
	memset(&ev, 0, sizeof(ev));
	input_init(&p);
	feed(&p, &ev, "q \033[C\033[D\033OA\033[B\033[5~\033[6~\003");
	ASSERT_EQ(9, ev.nkeys);
	ASSERT_EQ('q', ev.keys[0]);
	ASSERT_EQ(' ', ev.keys[1]);
	ASSERT_EQ(KEY_RIGHT, ev.keys[2]);
	ASSERT_EQ(KEY_LEFT, ev.keys[3]);
	ASSERT_EQ(KEY_UP, ev.keys[4]);
	ASSERT_EQ(KEY_DOWN, ev.keys[5]);
	ASSERT_EQ(KEY_PAGE_UP, ev.keys[6]);
	ASSERT_EQ(KEY_PAGE_DOWN, ev.keys[7]);
	ASSERT_EQ(3, ev.keys[8]);
	ASSERT(!input_pending_escape(&p));
}

TEST(input_replies_between_keys)
{
	InputParser p;
	Events ev;
	memset(&ev, 0, sizeof(ev));
	input_init(&p);
	feed(&p, &ev, "a\033_Gi=7;OK seq=3 state=playing\033\\b\033[?62;22c"
	    "\033[6;20;10tc");
	ASSERT_EQ(3, ev.nkeys);
	ASSERT_EQ('a', ev.keys[0]);
	ASSERT_EQ('b', ev.keys[1]);
	ASSERT_EQ('c', ev.keys[2]);
	ASSERT_EQ(1, ev.napc);
	ASSERT_STR_EQ("i=7;OK seq=3 state=playing", ev.apc[0]);
	ASSERT_EQ(1, ev.da1);
	ASSERT_EQ(10, ev.cell_w);
	ASSERT_EQ(20, ev.cell_h);
}

/* Pipe mode: an owner's commands arrive as "ESC _ ! ... ESC \\" among the
 * terminal replies it relays; neither is taken for the other. */
TEST(input_pipe_commands_between_replies)
{
	const char *s = "\033_!seek 12.5\033\\\033_Gi=7;OK seq=3\033\\"
	    "\033_!toggle\033\\";
	size_t split, n = strlen(s);

	for (split = 1; split < n; split++) {
		InputParser p;
		Events ev;
		memset(&ev, 0, sizeof(ev));
		input_init(&p);
		input_feed(&p, (const unsigned char *)s, split, collect, &ev);
		input_feed(&p, (const unsigned char *)s + split, n - split,
		    collect, &ev);
		ASSERT_EQ(2, ev.ncommand);
		ASSERT_STR_EQ("seek 12.5", ev.command[0]);
		ASSERT_STR_EQ("toggle", ev.command[1]);
		ASSERT_EQ(1, ev.napc);
		ASSERT_STR_EQ("i=7;OK seq=3", ev.apc[0]);
		ASSERT_EQ(0, ev.nkeys);
	}
}

TEST(input_reply_split_anywhere)
{
	const char *s = "\033_Gi=9;OK seq=12 state=paused pos=5\033\\";
	size_t split, n = strlen(s);

	for (split = 1; split < n; split++) {
		InputParser p;
		Events ev;
		memset(&ev, 0, sizeof(ev));
		input_init(&p);
		input_feed(&p, (const unsigned char *)s, split, collect, &ev);
		/* A pause here must not turn the ESC into a key. */
		input_timeout(&p, collect, &ev);
		input_feed(&p, (const unsigned char *)s + split, n - split, collect, &ev);
		if (split == 1) {
			/* ESC alone in its own read, then a timeout: that is the
			 * Escape key by definition; covered below. */
			continue;
		}
		ASSERT_EQ(0, ev.nkeys);
		ASSERT_EQ(1, ev.napc);
	}
	/* Byte by byte without timeouts. */
	{
		InputParser p;
		Events ev;
		size_t i;
		memset(&ev, 0, sizeof(ev));
		input_init(&p);
		for (i = 0; i < n; i++)
			input_feed(&p, (const unsigned char *)s + i, 1, collect, &ev);
		ASSERT_EQ(0, ev.nkeys);
		ASSERT_EQ(1, ev.napc);
	}
}

TEST(input_lone_escape)
{
	InputParser p;
	Events ev;
	memset(&ev, 0, sizeof(ev));
	input_init(&p);
	feed(&p, &ev, "\033");
	ASSERT(input_pending_escape(&p));
	ASSERT_EQ(0, ev.nkeys);
	input_timeout(&p, collect, &ev);
	ASSERT_EQ(1, ev.nkeys);
	ASSERT_EQ(KEY_ESCAPE, ev.keys[0]);
	/* ESC then the rest of an arrow in the next read: an arrow. */
	memset(&ev, 0, sizeof(ev));
	feed(&p, &ev, "\033");
	feed(&p, &ev, "[C");
	ASSERT_EQ(1, ev.nkeys);
	ASSERT_EQ(KEY_RIGHT, ev.keys[0]);
	/* ESC at the end of a longer read is never the Escape key. */
	memset(&ev, 0, sizeof(ev));
	feed(&p, &ev, "x\033");
	input_timeout(&p, collect, &ev);
	ASSERT_EQ(1, ev.nkeys);
	ASSERT_EQ('x', ev.keys[0]);
}

TEST(input_oversized_apc_dropped)
{
	InputParser p;
	Events ev;
	char *big = malloc(INPUT_APC_MAX + 64);
	memset(&ev, 0, sizeof(ev));
	input_init(&p);
	memcpy(big, "\033_G", 3);
	memset(big + 3, 'A', INPUT_APC_MAX + 20);
	memcpy(big + 3 + INPUT_APC_MAX + 20, "\033\\z", 4);
	feed(&p, &ev, big);
	free(big);
	ASSERT_EQ(0, ev.napc);
	ASSERT_EQ(1, ev.nkeys);
	ASSERT_EQ('z', ev.keys[0]);
}

TEST(input_other_strings_skipped)
{
	InputParser p;
	Events ev;
	memset(&ev, 0, sizeof(ev));
	input_init(&p);
	feed(&p, &ev, "\033]11;rgb:0000/0000/0000\033\\\033P1$r0m\033\\k");
	ASSERT_EQ(1, ev.nkeys);
	ASSERT_EQ('k', ev.keys[0]);
}

TEST(reply_status_fields)
{
	Reply r;
	const char *s = "i=1234;OK seq=7 state=playing pos=1234567 vbuf=3400000 "
	    "abuf=3412000 shown=36 dropped=2 rx=482113 error=EBADMSG";
	ASSERT(reply_parse(s, strlen(s), &r));
	ASSERT(r.id == 1234);
	ASSERT(r.ok);
	ASSERT(r.has_seq && r.seq == 7);
	ASSERT_EQ(1, r.state);
	ASSERT(r.pos == 1234567);
	ASSERT(r.vbuf == 3400000);
	ASSERT(r.abuf == 3412000);
	ASSERT(r.shown == 36 && r.dropped == 2 && r.rx == 482113);
	ASSERT_STR_EQ("EBADMSG", r.error);
}

TEST(reply_negative_pos_and_errors)
{
	Reply r;
	const char *s = "i=5;OK seq=1 state=buffering pos=-1 vbuf=-1";
	const char *e = "i=5,p=1;ENOENT:video not found";
	const char *caps = "i=5;OK video=h264 audio=aac max=4096x4096";
	ASSERT(reply_parse(s, strlen(s), &r));
	ASSERT(r.pos == -1);
	ASSERT_EQ(0, r.state);
	ASSERT(reply_parse(e, strlen(e), &r));
	ASSERT(!r.ok);
	ASSERT(!r.has_seq);
	ASSERT(!strncmp(r.message, "ENOENT", 6));
	ASSERT(reply_parse(caps, strlen(caps), &r));
	ASSERT(r.ok && r.video_h264 && r.audio_aac);
	ASSERT(!reply_parse("OK", 2, &r));
}

/* Framing */

static size_t
b64decode(const char *in, size_t n, unsigned char *out)
{
	static const char *t =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned v = 0;
	int bits = 0;
	size_t i, o = 0;
	for (i = 0; i < n && in[i] != '='; i++) {
		v = v << 6 | (unsigned)(strchr(t, in[i]) - t);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (unsigned char)(v >> bits);
		}
	}
	return o;
}

TEST(base64_vectors)
{
	char out[16];
	size_t n;
	n = base64_encode((const unsigned char *)"f", 1, out);
	ASSERT(n == 4 && !memcmp(out, "Zg==", 4));
	n = base64_encode((const unsigned char *)"fo", 2, out);
	ASSERT(n == 4 && !memcmp(out, "Zm8=", 4));
	n = base64_encode((const unsigned char *)"foobar", 6, out);
	ASSERT(n == 8 && !memcmp(out, "Zm9vYmFy", 8));
	ASSERT(base64_length(0) == 0 && base64_length(4) == 8);
}

TEST(apc_small_and_empty)
{
	char out[128];
	size_t n = apc_build(out, "a=v,i=3,k=q", NULL, 0,
	    (const unsigned char *)"seq=1", 5, OUT_CHUNK);
	out[n] = '\0';
	ASSERT_STR_EQ("\033_Ga=v,i=3,k=q;c2VxPTE=\033\\", out);
	ASSERT(n == apc_length(11, 5, OUT_CHUNK));
	n = apc_build(out, "a=v,i=3,k=e,q=1", NULL, 0, NULL, 0, OUT_CHUNK);
	out[n] = '\0';
	ASSERT_STR_EQ("\033_Ga=v,i=3,k=e,q=1\033\\", out);
	ASSERT(n == apc_length(15, 0, OUT_CHUNK));
}

TEST(apc_chunked_packet_round_trip)
{
	size_t length = 40000, n, chunk = 4096, chunks = 0;
	unsigned char *payload = malloc(length), header[10], *decoded;
	char *out, *p, *end;
	size_t dn = 0;
	int last_more = -1;

	for (n = 0; n < length; n++)
		payload[n] = (unsigned char)(n * 7 + 3);
	packet_header(header, 0, 1, -1234567);
	ASSERT(header[0] == 0 && header[1] == 1);
	ASSERT(header[2] == 0xff && header[9] == (unsigned char)(-1234567 & 0xff));
	out = malloc(apc_length(16, length + 10, chunk));
	decoded = malloc(length + 64);
	n = apc_build(out, "a=v,i=9,k=d,q=2", header, 10, payload, length, chunk);
	ASSERT(n == apc_length(15, length + 10, chunk));
	/* Walk the APCs: controls only on the first, m=1 on all but the last. */
	p = out;
	end = out + n;
	while (p < end) {
		char *semi, *st;
		ASSERT(!memcmp(p, "\033_G", 3));
		semi = memchr(p, ';', (size_t)(end - p));
		st = memchr(p + 1, '\033', (size_t)(end - p - 1));
		ASSERT(semi && st && st[1] == '\\');
		if (chunks == 0)
			ASSERT(!memcmp(p + 3, "a=v,i=9,k=d,q=2,m=1;", 20));
		else
			ASSERT(!memcmp(p + 3, "m=", 2));
		last_more = semi[-1] == '1';
		ASSERT(st - semi - 1 <= (long)chunk);
		dn += b64decode(semi + 1, (size_t)(st - semi - 1), decoded + dn);
		p = st + 2;
		chunks++;
	}
	ASSERT(chunks == (base64_length(length + 10) + chunk - 1) / chunk);
	ASSERT_EQ(0, last_more);
	ASSERT(dn == length + 10);
	ASSERT(!memcmp(decoded, header, 10));
	ASSERT(!memcmp(decoded + 10, payload, length));
	free(payload);
	free(out);
	free(decoded);
}

static OutMsg *
msg(int kind, int tag)
{
	OutMsg *m = out_msg_new(kind, 10);
	m->tag = tag;
	return m;
}

TEST(queue_controls_overtake_media)
{
	OutQueue q;
	OutMsg *m1 = msg(OUT_MEDIA, 1), *m2 = msg(OUT_MEDIA, 2);
	memset(&q, 0, sizeof(q));
	out_push(&q, m1);
	out_push(&q, m2);
	out_push(&q, msg(OUT_CONTROL, 3));
	ASSERT_EQ(3, q.head->tag);
	ASSERT_EQ(1, q.head->next->tag);
	/* A started message is never split. */
	out_advance(&q, 10);	/* control done */
	out_advance(&q, 4);	/* m1 started */
	out_push(&q, msg(OUT_CONTROL, 4));
	out_push(&q, msg(OUT_PING, 5));
	ASSERT_EQ(1, q.head->tag);
	ASSERT_EQ(4, q.head->next->tag);
	ASSERT_EQ(5, q.head->next->next->tag);
	ASSERT_EQ(2, q.head->next->next->next->tag);
	ASSERT(q.tail == m2);
	ASSERT(q.bytes == 6 + 10 + 10 + 10);
	ASSERT(q.media_bytes == 16);
	/* A seek drops queued media but not the started one. */
	ASSERT(out_drop_media(&q) == 10);
	ASSERT(q.media_count == 1 && q.media_bytes == 6);
	ASSERT(q.tail->tag == 5);
	out_clear(&q);
	ASSERT(!q.head && !q.tail && !q.bytes);
}

TEST(queue_drop_tag)
{
	OutQueue q;
	memset(&q, 0, sizeof(q));
	out_push(&q, msg(OUT_CONTROL, 1));
	out_push(&q, msg(OUT_CONTROL, 0));
	out_push(&q, msg(OUT_CONTROL, 1));
	out_push(&q, msg(OUT_MEDIA, 0));
	out_drop_tag(&q, 1);
	ASSERT(q.head->tag == 0 && q.head->kind == OUT_CONTROL);
	ASSERT(q.head->next->kind == OUT_MEDIA && !q.head->next->next);
	ASSERT(q.bytes == 20);
	out_push(&q, msg(OUT_CONTROL, 7));
	ASSERT(q.head->next->tag == 7);
	out_clear(&q);
}

/* Flow control against a simulated link. */

typedef struct {
	double t;
	double capacity;	/* bytes per second, may change */
	double owd;
	double link_free;
	double replies[4096];
	uint32_t reply_seq[4096];
	int nreplies;
	double stall_from, stall_to;	/* terminal answers nothing then */
	double app_rate;	/* 0: always has data */
	double burst;		/* the terminal reads (and answers) this many
				 * bytes at a time */
	double app_credit;
	double min_rate, max_rate;	/* estimate range after warmup */
} Sim;

static void
sim_write(Sim *s, Flow *f, size_t n)
{
	double start = s->link_free > s->t ? s->link_free : s->t;
	s->link_free = start + n / s->capacity;
	flow_wrote(f, n);
}

static void
sim_ping(Sim *s, Flow *f)
{
	uint32_t seq = flow_next_seq(f);
	double arrive, reply;
	flow_ping_written(f, seq, s->t);
	sim_write(s, f, 40);
	arrive = s->link_free + s->owd;
	if (s->burst > 0) {
		double q = s->burst / s->capacity;
		arrive = ceil(arrive / q) * q;
	}
	reply = arrive + s->owd;
	if (arrive >= s->stall_from && arrive < s->stall_to)
		reply = s->stall_to + s->owd;
	if (s->nreplies < 4096) {
		s->replies[s->nreplies] = reply;
		s->reply_seq[s->nreplies] = seq;
		s->nreplies++;
	}
}

static void
sim_run(Sim *s, Flow *f, double until, double track_from)
{
	for (; s->t < until; s->t += 0.001) {
		int i, k = 0;
		/* replies due */
		for (i = 0; i < s->nreplies; i++) {
			if (s->replies[i] <= s->t)
				flow_ack(f, s->reply_seq[i], s->t);
			else {
				s->replies[k] = s->replies[i];
				s->reply_seq[k] = s->reply_seq[i];
				k++;
			}
		}
		s->nreplies = k;
		if (s->app_rate > 0)
			s->app_credit += s->app_rate * 0.001;
		for (;;) {
			int have = s->app_rate <= 0 || s->app_credit >= 4096;
			if (flow_ping_due(f, s->t, have))
				sim_ping(s, f);
			if (!have) {
				if (flow_may_send(f))
					flow_idle(f);
				break;
			}
			if (!flow_may_send(f))
				break;
			sim_write(s, f, 4096);
			if (s->app_rate > 0)
				s->app_credit -= 4096;
		}
		if (getenv("SIM_TRACE") && fmod(s->t + 1e-9, 0.25) < 0.001)
			printf("t=%.2f rate=%.0f win=%.0f inflight=%llu srtt=%.3f\n", s->t, f->rate, flow_window(f), (unsigned long long)flow_inflight(f), f->srtt);
		if (s->t >= track_from) {
			if (f->rate < s->min_rate) s->min_rate = f->rate;
			if (f->rate > s->max_rate) s->max_rate = f->rate;
		}
	}
}

static void
sim_init(Sim *s, double capacity, double rtt)
{
	memset(s, 0, sizeof(*s));
	s->capacity = capacity;
	s->owd = rtt / 2;
	s->min_rate = 1e18;
	s->stall_from = s->stall_to = -1;
}

TEST(flow_window_bounds)
{
	Flow f;
	flow_init(&f, 0, 0.15, 1e3);
	ASSERT(fabs(flow_window(&f) - FLOW_MIN_WINDOW) < 1);
	/* 1 MB/s, 150 ms: one RTT + 80 ms of the link, and a ping's worth. */
	f.rate = f.throughput = 1e6;
	ASSERT(fabs(flow_window(&f) - (230e3 + FLOW_PING_BYTES)) < 1);
	f.min_rtt = 0.01;	/* local: at least 200 ms */
	ASSERT(fabs(flow_window(&f) - (200e3 + FLOW_PING_BYTES)) < 1);
	f.min_rtt = 0.4;	/* long RTT: the window covers it */
	ASSERT(flow_window(&f) > 480e3);
	/* A stale or guessed rate: no more than twice what got through. */
	f.throughput = 100e3;
	ASSERT(flow_window(&f) < 200e3 * 0.48 + FLOW_PING_BYTES + 1);
}

TEST(flow_acks_bound_inflight)
{
	Flow f;
	uint32_t a, b;
	flow_init(&f, 0, 0.1, 1e6);
	flow_wrote(&f, 1000);
	a = flow_next_seq(&f);
	flow_ping_written(&f, a, 0.0);
	flow_wrote(&f, 2000);
	b = flow_next_seq(&f);
	flow_ping_written(&f, b, 0.01);
	flow_wrote(&f, 500);
	ASSERT(flow_inflight(&f) == 3500);
	ASSERT(flow_ack(&f, a, 0.1));
	ASSERT(flow_inflight(&f) == 2500);
	ASSERT(!flow_ack(&f, a, 0.11));	/* already done */
	ASSERT(flow_ack(&f, b, 0.12));
	ASSERT(flow_inflight(&f) == 500);
	ASSERT(fabs(f.last_rtt - 0.11) < 1e-9);
	ASSERT(!flow_ack(&f, 99, 0.2));
}

TEST(flow_bounds_unanswered_pings)
{
	/* A terminal that stops answering: we stop asking. */
	Flow f;
	double t;
	int sent = 0;
	flow_init(&f, 0, 0.05, 10e6);
	for (t = 0; t < 0.9; t += 0.001) {
		flow_wrote(&f, 50000);
		if (flow_ping_due(&f, t, 1)) {
			flow_ping_written(&f, flow_next_seq(&f), t);
			sent++;
		}
	}
	ASSERT_EQ(FLOW_OUTSTANDING, sent);
	ASSERT(f.ping_count == FLOW_OUTSTANDING);
	/* One answer frees one slot. */
	ASSERT(flow_ack(&f, 1, 0.9));
	ASSERT(flow_ping_due(&f, 0.9, 1));
}

TEST(flow_lost_replies_do_not_stall)
{
	/* Replies to the first 16 pings never come: after a while it asks
	 * again, and the next reply acknowledges everything before it. */
	Flow f;
	double t;
	uint32_t last = 0;
	flow_init(&f, 0, 0.05, 1e6);
	for (t = 0; t < 3; t += 0.001) {
		flow_wrote(&f, 1000);
		if (flow_ping_due(&f, t, 1)) {
			last = flow_next_seq(&f);
			flow_ping_written(&f, last, t);
		}
	}
	ASSERT(last > FLOW_OUTSTANDING);
	/* Not a flood once the first ones are given up. */
	ASSERT(last < 2 * FLOW_OUTSTANDING + 3 / 0.033);
	ASSERT(f.ping_count <= FLOW_OUTSTANDING);
	ASSERT(flow_ack(&f, last, 3.0));
	ASSERT(f.acked >= 1000000 && flow_inflight(&f) < 1100000);
}

TEST(flow_measures_window_limited_link)
{
	/* 3 Mbit/s wire (375 kB/s), 150 ms RTT, starting from a low guess. */
	Sim s;
	Flow f;
	sim_init(&s, 375e3, 0.15);
	flow_init(&f, 0, 0.15, 100e3);
	sim_run(&s, &f, 6.0, 3.0);
	ASSERT(s.min_rate > 0.85 * 375e3);
	ASSERT(s.max_rate < 1.2 * 375e3);
	/* In flight stays near the window: ~250 ms of the link. */
	ASSERT(flow_window(&f) < 0.3 * 1.2 * 375e3 + 1);
}

TEST(flow_bursty_replies_do_not_inflate_the_rate)
{
	/* Replies come in 64 KiB bursts (175 ms of this link). */
	Sim s;
	Flow f;
	sim_init(&s, 375e3, 0.15);
	s.burst = 65536;
	flow_init(&f, 0, 0.15, 375e3);
	sim_run(&s, &f, 10.0, 3.0);
	ASSERT(s.max_rate < 1.1 * 375e3);
	ASSERT(s.min_rate > 0.85 * 375e3);
}

TEST(flow_follows_a_drop_and_recovery)
{
	Sim s;
	Flow f;
	sim_init(&s, 375e3, 0.15);
	flow_init(&f, 0, 0.15, 375e3);
	sim_run(&s, &f, 4.0, 4.0);
	s.capacity = 125e3;	/* 3 -> 1 Mbit/s */
	sim_run(&s, &f, 6.0, 99);
	ASSERT(f.rate < 1.3 * 125e3);
	s.min_rate = 1e18;
	s.max_rate = 0;
	sim_run(&s, &f, 8.0, 6.0);
	ASSERT(s.min_rate > 0.8 * 125e3);
	s.capacity = 375e3;	/* back up: found again within seconds */
	sim_run(&s, &f, 12.0, 99);
	ASSERT(f.rate > 0.85 * 375e3);
}

TEST(flow_guess_too_high_and_fast_link_turning_slow)
{
	/* Local start: a 40 MB/s guess, sending 1 MB/s (app-limited) over a
	 * fast link, which then turns into 3 Mbit/s. */
	Sim s;
	Flow f;
	sim_init(&s, 50e6, 0.002);
	flow_init(&f, 0, 0.002, 40e6);
	s.app_rate = 1e6;
	sim_run(&s, &f, 3.0, 99);
	/* The window follows what got through, not the guess. */
	ASSERT(flow_window(&f) < 2 * 1e6 * 0.2 + 64 * 1024);
	s.capacity = 375e3;
	s.app_rate = 0;		/* now it has a backlog */
	sim_run(&s, &f, 5.0, 99);
	ASSERT(f.rate < 1.5 * 375e3);
	ASSERT(flow_inflight(&f) < 375e3 * 0.5);
}

TEST(flow_ignores_app_limited_intervals)
{
	/* Sending 50 kB/s on a 375 kB/s link must not lower the estimate. */
	Sim s;
	Flow f;
	sim_init(&s, 375e3, 0.15);
	flow_init(&f, 0, 0.15, 375e3);
	s.app_rate = 50e3;
	sim_run(&s, &f, 5.0, 0.5);
	ASSERT(s.min_rate >= 375e3 * 0.99);
}

TEST(flow_survives_a_terminal_stall)
{
	/* The terminal answers nothing for 400 ms, then all at once. */
	Sim s;
	Flow f;
	sim_init(&s, 375e3, 0.15);
	flow_init(&f, 0, 0.15, 375e3);
	sim_run(&s, &f, 4.0, 99);
	s.stall_from = 4.0;
	s.stall_to = 4.4;
	sim_run(&s, &f, 8.0, 4.0);
	ASSERT(s.min_rate > 0.6 * 375e3);
	ASSERT(f.rate > 0.85 * 375e3);
}

/* Adaptation */

TEST(adapt_ladder_never_upscales)
{
	Rung r[ADAPT_RUNGS];
	int n = adapt_ladder(r, ADAPT_RUNGS, 1920, 1080), i;
	ASSERT(n > 5);
	ASSERT(r[n - 1].width == 1920 && r[n - 1].height == 1080);
	for (i = 0; i < n; i++) {
		ASSERT(r[i].width % 2 == 0 && r[i].height % 2 == 0);
		ASSERT(r[i].width <= 1920 && r[i].height <= 1080);
		if (i)
			ASSERT(r[i].height > r[i - 1].height);
	}
	/* A placement smaller than every standard rung: just itself. */
	n = adapt_ladder(r, ADAPT_RUNGS, 101, 57);
	ASSERT(r[n - 1].width == 100 && r[n - 1].height == 56);
}

TEST(adapt_video_kbps_counts_overhead)
{
	Adapt a;
	Rung r[ADAPT_RUNGS];
	int n = adapt_ladder(r, ADAPT_RUNGS, 1920, 1080);
	double kbps;
	adapt_init(&a, 0, r, n, 30, 1e6, 128, 0);
	a.ahead_target = 3;
	/* 1 MB/s of wire carries at most 6 Mbit/s of payload. */
	kbps = adapt_video_kbps(&a, 1e6, 5);
	ASSERT(kbps < 6000 * 0.86 - 128);
	ASSERT(kbps > 6000 * 0.80 - 128);
	/* Less of it when the lead is short. */
	ASSERT(adapt_video_kbps(&a, 1e6, 0.5) < 0.75 * kbps);
}

TEST(adapt_steps_down_fast_and_up_slowly)
{
	Adapt a;
	Flow f;
	Rung r[ADAPT_RUNGS];
	int n = adapt_ladder(r, ADAPT_RUNGS, 1920, 1080), start;
	double t;
	flow_init(&f, 0, 0.15, 375e3);
	adapt_init(&a, 0, r, n, 30, f.rate, 96, 0);
	a.ahead_target = 3;
	start = a.rung;
	ASSERT(r[start].height >= 480 && r[start].height <= 720);
	/* Link falls to 0.5 Mbit/s: a smaller rung within ~1.5 s. */
	f.rate = 62.5e3;
	for (t = 0; t < 1.5; t += 0.1)
		adapt_update(&a, &f, 10 + t, 3, 0);
	ASSERT(a.rung < start);
	ASSERT(r[a.rung].height <= 360);
	ASSERT(a.kbps < 450);
	/* Back to 3 Mbit/s: not up at once, but within a few seconds. */
	f.rate = 375e3;
	start = a.rung;
	adapt_update(&a, &f, 12, 3, 0);
	ASSERT_EQ(start, a.rung);
	for (t = 0; t < 7; t += 0.1)
		adapt_update(&a, &f, 12 + t, 3, 0);
	ASSERT(a.rung > start);
	ASSERT(r[a.rung].height >= 540);
}

TEST(adapt_presumes_more_when_the_buffer_is_full)
{
	/* App-limited with a full lead: the presumed rate grows until the
	 * encoder can outrun the link; not while paused, not at the top. */
	Adapt a;
	Flow f;
	Rung r[ADAPT_RUNGS];
	int n = adapt_ladder(r, ADAPT_RUNGS, 1920, 1080);
	double t, start;
	flow_init(&f, 0, 0.15, 125e3);
	adapt_init(&a, 0, r, n, 30, f.rate, 96, 0);
	a.ahead_target = 3;
	a.ahead_cap = 5;
	start = a.kbps;
	for (t = 0; t < 5; t += 0.1)
		adapt_update(&a, &f, t, 5, 1);
	ASSERT(f.rate > 2 * 125e3);
	ASSERT(a.kbps > 1.5 * start);
	f.rate = 125e3;
	a.paused = 1;
	for (t = 5; t < 10; t += 0.1)
		adapt_update(&a, &f, t, 5, 1);
	ASSERT(f.rate < 1.01 * 125e3);
	/* Lead not full: no presuming. */
	a.paused = 0;
	for (t = 10; t < 15; t += 0.1)
		adapt_update(&a, &f, t, 3, 1);
	ASSERT(f.rate < 1.01 * 125e3);
}

TEST(adapt_probes_when_app_limited)
{
	Adapt a;
	Flow f;
	Rung r[ADAPT_RUNGS];
	int n = adapt_ladder(r, ADAPT_RUNGS, 1920, 1080);
	double t;
	int probed = 0;
	flow_init(&f, 0, 0.15, 100e3);
	adapt_init(&a, 0, r, n, 30, f.rate, 96, 0);
	for (t = 0; t < 10; t += 0.1) {
		adapt_update(&a, &f, t, 5, 1);
		if (adapt_probing(&a, t)) {
			probed = 1;
			ASSERT(f.window_gain > 1);
		}
	}
	ASSERT(probed);
	/* Not while window-limited: the link is being measured anyway. */
	flow_init(&f, 0, 0.15, 100e3);
	adapt_init(&a, 0, r, n, 30, f.rate, 96, 0);
	for (t = 0; t < 10; t += 0.1) {
		adapt_update(&a, &f, t, 5, 0);
		ASSERT(!adapt_probing(&a, t));
	}
}

int
main(void)
{
	printf("st-play tests\n");
	RUN_TEST(input_keys);
	RUN_TEST(input_replies_between_keys);
	RUN_TEST(input_reply_split_anywhere);
	RUN_TEST(input_pipe_commands_between_replies);
	RUN_TEST(input_lone_escape);
	RUN_TEST(input_oversized_apc_dropped);
	RUN_TEST(input_other_strings_skipped);
	RUN_TEST(reply_status_fields);
	RUN_TEST(reply_negative_pos_and_errors);
	RUN_TEST(base64_vectors);
	RUN_TEST(apc_small_and_empty);
	RUN_TEST(apc_chunked_packet_round_trip);
	RUN_TEST(queue_controls_overtake_media);
	RUN_TEST(queue_drop_tag);
	RUN_TEST(flow_window_bounds);
	RUN_TEST(flow_acks_bound_inflight);
	RUN_TEST(flow_bounds_unanswered_pings);
	RUN_TEST(flow_lost_replies_do_not_stall);
	RUN_TEST(flow_measures_window_limited_link);
	RUN_TEST(flow_bursty_replies_do_not_inflate_the_rate);
	RUN_TEST(flow_follows_a_drop_and_recovery);
	RUN_TEST(flow_guess_too_high_and_fast_link_turning_slow);
	RUN_TEST(flow_ignores_app_limited_intervals);
	RUN_TEST(flow_survives_a_terminal_stall);
	RUN_TEST(adapt_ladder_never_upscales);
	RUN_TEST(adapt_video_kbps_counts_overhead);
	RUN_TEST(adapt_steps_down_fast_and_up_slowly);
	RUN_TEST(adapt_presumes_more_when_the_buffer_is_full);
	RUN_TEST(adapt_probes_when_app_limited);
	return test_summary();
}

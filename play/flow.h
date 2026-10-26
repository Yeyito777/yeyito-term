/* st-play flow control and adaptation (docs/video.md, "Flow control for
 * senders").  Pure logic with injected time so the tests can drive it.
 *
 * Flow: every k=q ping records how many bytes were written before it; its
 * reply means all of them reached the terminal.  Bytes written after the
 * newest acknowledged ping are "in flight" and kept below a window of about
 * 250 ms at the delivery rate.  The delivery rate is estimated from the
 * acknowledgement rate over spans in which the sender was window-limited
 * (it always had media to send), not app-limited (BBR-style marking).
 *
 * Adapt: turns the delivery rate and the media buffered ahead of the
 * playback position into an encoder bitrate and a resolution step. */
#ifndef PLAY_FLOW_H
#define PLAY_FLOW_H

#include <stddef.h>
#include <stdint.h>

#define FLOW_MIN_WINDOW   (64.0 * 1024)
#define FLOW_PING_BYTES   (32.0 * 1024)
#define FLOW_PING_SENDING 0.1	/* seconds between pings while sending */
#define FLOW_PING_IDLE    0.25	/* keepalive pings for position updates */
#define FLOW_PINGS        2048
/* Unanswered pings at most.  Their replies queue in the terminal's tty
 * buffer toward us (4 KiB on Linux) while we encode; a terminal whose reply
 * write blocks may stall or worse (st re-enters its pty reader), so keep
 * them well under it: 16 x ~115 bytes. */
#define FLOW_OUTSTANDING  16
#define FLOW_ACKS         256
#define FLOW_AVERAGE      3.0	/* seconds of window-limited acks averaged */

typedef struct {
	uint32_t seq;
	uint64_t offset;	/* bytes written before the ping */
	double sent;
	int limited;		/* not written in an app-limited phase */
} FlowPing;

typedef struct {
	uint64_t offset;
	double time;
	int limited;
} FlowAck;

typedef struct {
	uint64_t written;	/* bytes written to the terminal */
	uint64_t acked;		/* offset of the newest acknowledged ping */
	uint32_t next_seq;
	FlowPing pings[FLOW_PINGS];
	unsigned ping_first, ping_count;
	FlowAck acks[FLOW_ACKS];
	unsigned ack_next, ack_count;

	double rate;		/* delivery rate estimate, wire bytes per second */
	double min_rtt, srtt, last_rtt;
	double window_gain;	/* > 1 while probing */
	/* Bytes written up to the last time the sender had nothing to send
	 * with the window open; pings written before acks pass it measure the
	 * application, not the link (BBR's app-limited marking).  0: none. */
	uint64_t app_limited_until;
	double last_ping_time;
	uint64_t last_ping_offset;
	int low_count;		/* consecutive valid samples well below rate */
	double low_max, low_since;
	double average_since;	/* the average ignores acks before (a drop) */
	double ack_burst;	/* bytes acknowledged by one burst of replies */
	double throughput;	/* ack rate over the last 0.5 s, limited or not */
	double burst_bytes, last_ack_time;
	/* Diagnostics */
	double last_sample;
	int last_sample_valid;
	unsigned long valid_samples;
} Flow;

void flow_init(Flow *flow, double now, double rtt, double rate);
double flow_window(const Flow *flow);
uint64_t flow_inflight(const Flow *flow);
int flow_may_send(const Flow *flow);
void flow_wrote(Flow *flow, size_t bytes);
/* Nothing to send while the window was open. */
void flow_idle(Flow *flow);
/* `sending`: media is queued or being produced. */
int flow_ping_due(const Flow *flow, double now, int sending);
uint32_t flow_next_seq(Flow *flow);
/* The ping with `seq` is about to be written (all earlier bytes are). */
void flow_ping_written(Flow *flow, uint32_t seq, double now);
/* Reply to ping `seq` arrived at `now`.  Returns 1 if it was outstanding. */
int flow_ack(Flow *flow, uint32_t seq, double now);
/* Forget outstanding pings (after a reopen the ids restart). */
void flow_forget(Flow *flow);

/* Resolution ladder and bitrate. */
#define ADAPT_RUNGS 24

typedef struct {
	int width, height;
} Rung;

typedef struct {
	Rung rungs[ADAPT_RUNGS];
	int count;
	int rung;		/* current */
	double fps;
	double kbps;		/* current video bitrate target */
	double min_kbps, max_kbps;
	double audio_kbps;
	double packets_per_second;
	double up_since, down_since;
	double last_change, last_rung_change;
	double probe_until, next_probe;
	double probe_start_rate;
	double ahead_target;	/* seconds of lead at which the margin is smallest */
	double panic_since;
	int fixed;		/* --bitrate: no adaptation */
	/* Diagnostics */
	double usable_kbps;
} Adapt;

/* Bits per pixel per frame thresholds. */
#define ADAPT_BPP_UP    0.075
#define ADAPT_BPP_DOWN  0.045
#define ADAPT_BPP_PANIC 0.025
#define ADAPT_BPP_MAX   0.20
#define ADAPT_UP_HOLD   3.0
#define ADAPT_DOWN_HOLD 1.0

/* Rungs of the given aspect no larger than max_w x max_h, smallest first
 * (even sizes).  Returns the count. */
int adapt_ladder(Rung *rungs, int max, int max_width, int max_height);
void adapt_init(Adapt *adapt, double now, const Rung *rungs, int count,
		double fps, double rate, double audio_kbps, double max_kbps);
/* Video kbps a wire rate can carry (base64, APC and packet overhead). */
double adapt_video_kbps(const Adapt *adapt, double wire_rate, double ahead);
/* Called every ~100 ms.  `ahead`: seconds of media sent beyond the
 * playback position; `app_limited`: the sender had nothing to send
 * (ahead budget full).  Returns 1 if the rung changed (reopen the
 * encoder), and sets adapt->kbps. */
int adapt_update(Adapt *adapt, Flow *flow, double now, double ahead,
		int app_limited);
int adapt_probing(const Adapt *adapt, double now);

#endif

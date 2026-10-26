/* st-play flow control and adaptation (see flow.h). */
#include "flow.h"

#include <math.h>
#include <string.h>

void
flow_init(Flow *flow, double now, double rtt, double rate)
{
	memset(flow, 0, sizeof(*flow));
	flow->next_seq = 1;
	flow->rate = rate;
	flow->min_rtt = flow->srtt = flow->last_rtt = rtt > 0 ? rtt : 0.05;
	flow->window_gain = 1.0;
	flow->last_ping_time = now;
}

/* The rate the window may plan for: never more than twice what actually
 * got through lately.  The estimate can be a guess or stale (app-limited
 * for long), and a window sized for it would bury a link that just got
 * slow under seconds of queue.  Sending window-limited doubles it each
 * round trip. */
static double
window_rate(const Flow *flow)
{
	return flow->rate < 2 * flow->throughput ? flow->rate :
	    2 * flow->throughput;
}

/* Bytes between pings: an acknowledgement covers this much at once. */
static double
ping_bytes(const Flow *flow)
{
	double bytes = window_rate(flow) * 0.025;
	return bytes > FLOW_PING_BYTES ? bytes : FLOW_PING_BYTES;
}

/* The bytes the link holds in one round trip, the ones delivered but not
 * acknowledged yet (a ping's worth, or the burst in which replies arrive),
 * and ~80 ms (at least 200 ms in all) of queue so the link stays busy and a
 * faster link shows as a faster ack rate. */
double
flow_window(const Flow *flow)
{
	double span = flow->min_rtt + 0.08, window;

	if (span < 0.2)
		span = 0.2;
	window = window_rate(flow) * span + (flow->ack_burst > ping_bytes(flow) ?
	    flow->ack_burst : ping_bytes(flow));
	if (window < FLOW_MIN_WINDOW)
		window = FLOW_MIN_WINDOW;
	return window * flow->window_gain;
}

uint64_t
flow_inflight(const Flow *flow)
{
	return flow->written - flow->acked;
}

int
flow_may_send(const Flow *flow)
{
	return (double)flow_inflight(flow) < flow_window(flow);
}

void
flow_wrote(Flow *flow, size_t bytes)
{
	flow->written += bytes;
}

void
flow_idle(Flow *flow)
{
	flow->app_limited_until = flow->written ? flow->written : 1;
}

int
flow_ping_due(const Flow *flow, double now, int sending)
{
	double since = now - flow->last_ping_time;
	uint64_t bytes = flow->written - flow->last_ping_offset;

	if (flow->ping_count >= FLOW_OUTSTANDING)
		return 0;
	if ((double)bytes >= ping_bytes(flow))
		return 1;
	if (sending && bytes && since >= FLOW_PING_SENDING)
		return 1;
	return since >= FLOW_PING_IDLE;
}

uint32_t
flow_next_seq(Flow *flow)
{
	return flow->next_seq++;
}

void
flow_ping_written(Flow *flow, uint32_t seq, double now)
{
	FlowPing *ping;

	if (flow->ping_count >= FLOW_PINGS) {
		/* Should not happen (flow_ping_due refuses); drop the oldest. */
		flow->ping_first = (flow->ping_first + 1) % FLOW_PINGS;
		flow->ping_count--;
	}
	ping = &flow->pings[(flow->ping_first + flow->ping_count) % FLOW_PINGS];
	ping->seq = seq;
	ping->offset = flow->written;
	ping->sent = now;
	ping->limited = !flow->app_limited_until;
	flow->ping_count++;
	flow->last_ping_time = now;
	flow->last_ping_offset = flow->written;
}

/* Rate of the acks in (k, newest] where k is the newest ack at least
 * `span` older; *valid: every interval in it was window-limited. */
static double
span_rate(const Flow *flow, double span, int *valid)
{
	const FlowAck *last = &flow->acks[(flow->ack_next + FLOW_ACKS - 1) %
	    FLOW_ACKS];
	unsigned i;

	*valid = last->limited;
	for (i = 1; i < flow->ack_count; i++) {
		const FlowAck *old = &flow->acks[(flow->ack_next + FLOW_ACKS - 1 -
		    i) % FLOW_ACKS];
		if (last->time - old->time >= span) {
			if (last->offset <= old->offset || last->time <= old->time)
				return -1;
			return (double)(last->offset - old->offset) /
			    (last->time - old->time);
		}
		*valid &= old->limited;
	}
	return -1;
}

/* Acknowledged bytes per second over the window-limited intervals of the
 * last FLOW_AVERAGE seconds (since `since`).  Replies arrive in bursts (the
 * terminal reads, SSH forwards, in large chunks), so short spans overstate
 * the rate; summed over seconds the bursts cancel out. */
static double
average_rate(const Flow *flow, double now, double since)
{
	double bytes = 0, time = 0, cutoff = now - FLOW_AVERAGE;
	unsigned i;

	if (since > cutoff)
		cutoff = since;
	for (i = 0; i + 1 < flow->ack_count; i++) {
		const FlowAck *a = &flow->acks[(flow->ack_next + FLOW_ACKS - 1 -
		    i) % FLOW_ACKS];
		const FlowAck *b = &flow->acks[(flow->ack_next + FLOW_ACKS - 2 -
		    i) % FLOW_ACKS];
		if (b->time < cutoff)
			break;
		if (a->limited && a->offset > b->offset) {
			bytes += (double)(a->offset - b->offset);
			time += a->time - b->time;
		}
	}
	return time >= 0.3 ? bytes / time : -1;
}

/* A window this big holds a link of `rate` with its acknowledgement lag:
 * acks this slow are the link's doing, not the window's. */
static int
window_holds(const Flow *flow, double window, double rate)
{
	return window >= 1.2 * rate * flow->min_rtt + flow->ack_burst;
}

static void
estimate(Flow *flow, double now, double window, int window_full)
{
	/* min_rtt: srtt grows with the queue exactly when the link slows. */
	double hold = 2 * flow->min_rtt, average, recent, longer;
	int recent_valid, longer_valid;

	if (hold < 0.5)
		hold = 0.5;
	recent = span_rate(flow, 0.5, &recent_valid);
	flow->throughput = recent > 0 ? recent : 0;
	/* With the window full right now, slow acks mean a slow link even if
	 * the bytes went out after an app-limited pause. */
	if (window_full && recent > 0 && recent < 0.7 * flow->rate)
		recent_valid = 1;
	if (recent > 0 && recent < 0.7 * flow->rate &&
	    !window_holds(flow, window, recent))
		recent_valid = 0;
	flow->last_sample = recent;
	flow->last_sample_valid = recent_valid;
	if (recent > 0 && recent_valid) {
		flow->valid_samples++;
		if (recent < 0.7 * flow->rate) {
			/* Low for a while (not one stall's burst of acks): the
			 * link got slower.  Restart the average from here. */
			if (recent > flow->low_max)
				flow->low_max = recent;
			if (!flow->low_count)
				flow->low_since = now;
			if (++flow->low_count >= 3 && now - flow->low_since >= hold) {
				flow->rate = flow->low_max;
				flow->average_since = now - 0.5;
				flow->low_count = 0;
				flow->low_max = 0;
			}
			return;
		}
		flow->low_count = 0;
		flow->low_max = 0;
	}
	/* Much faster over 1.5 s: the link got faster; don't wait for the
	 * average.  (Long enough that bursts of replies can't fake a 30%
	 * jump.) */
	longer = span_rate(flow, 1.5, &longer_valid);
	if (longer > 1.3 * flow->rate && longer_valid) {
		flow->rate = longer;
		flow->average_since = now - 1.5;
		return;
	}
	average = average_rate(flow, now, flow->average_since);
	if (average > 0) {
		/* A small window (the start, after a pause) holds the rate
		 * down by itself; that is no reason to lower the estimate. */
		if (average >= flow->rate || window_holds(flow, window, average))
			flow->rate = average;
		return;
	}
	/* App-limited for long: a long span faster than the estimate still
	 * proves the link is at least that fast. */
	if (longer > flow->rate)
		flow->rate = longer;
}

int
flow_ack(Flow *flow, uint32_t seq, double now)
{
	unsigned i, found = FLOW_PINGS;
	int limited = 1;
	FlowPing ping;
	FlowAck *ack;
	double rtt;
	double window = flow_window(flow);
	int window_full = (double)flow_inflight(flow) >= 0.8 * window;

	for (i = 0; i < flow->ping_count; i++)
		if (flow->pings[(flow->ping_first + i) % FLOW_PINGS].seq == seq) {
			found = i;
			break;
		}
	if (found == FLOW_PINGS)
		return 0;
	/* Replies come in order; earlier pings without one count with it. */
	for (i = 0; i <= found; i++)
		limited &= flow->pings[(flow->ping_first + i) % FLOW_PINGS].limited;
	ping = flow->pings[(flow->ping_first + found) % FLOW_PINGS];
	flow->ping_first = (flow->ping_first + found + 1) % FLOW_PINGS;
	flow->ping_count -= found + 1;

	/* Replies arrive in bursts (the terminal reads its input, SSH
	 * forwards it, in chunks): track the bytes one burst acknowledges. */
	if (ping.offset > flow->acked) {
		double bytes = (double)(ping.offset - flow->acked);
		if (now - flow->last_ack_time < 0.003)
			flow->burst_bytes += bytes;
		else
			flow->burst_bytes = bytes;
		if (flow->burst_bytes > flow->ack_burst)
			flow->ack_burst = flow->burst_bytes;
		else
			flow->ack_burst = 0.97 * flow->ack_burst +
			    0.03 * flow->burst_bytes;
		if (flow->ack_burst > window_rate(flow) * 0.5 &&
		    flow->ack_burst > FLOW_MIN_WINDOW)
			flow->ack_burst = window_rate(flow) * 0.5 > FLOW_MIN_WINDOW ?
			    window_rate(flow) * 0.5 : FLOW_MIN_WINDOW;
		flow->acked = ping.offset;
	}
	if (flow->app_limited_until && flow->acked >= flow->app_limited_until)
		flow->app_limited_until = 0;
	flow->last_ack_time = now;
	rtt = now - ping.sent;
	if (rtt < 0)
		rtt = 0;
	flow->last_rtt = rtt;
	flow->srtt = 0.875 * flow->srtt + 0.125 * rtt;
	if (rtt < flow->min_rtt)
		flow->min_rtt = rtt;

	ack = &flow->acks[flow->ack_next];
	ack->offset = ping.offset;
	ack->time = now;
	ack->limited = limited;
	flow->ack_next = (flow->ack_next + 1) % FLOW_ACKS;
	if (flow->ack_count < FLOW_ACKS)
		flow->ack_count++;
	estimate(flow, now, window, window_full);
	return 1;
}

void
flow_forget(Flow *flow)
{
	flow->ping_count = 0;
	flow->ack_count = 0;
	flow->acked = flow->written;
}

/* Adaptation */

int
adapt_ladder(Rung *rungs, int max, int max_width, int max_height)
{
	static const int heights[] = { 144, 180, 240, 288, 360, 432, 480, 540,
	    576, 720, 810, 900, 1080, 1260, 1440, 1620, 1800, 2160, 2880 };
	int count = 0;
	size_t i;

	max_width &= ~1;
	max_height &= ~1;
	if (max_width < 16 || max_height < 16) {
		rungs[0].width = max_width < 16 ? 16 : max_width;
		rungs[0].height = max_height < 16 ? 16 : max_height;
		return 1;
	}
	for (i = 0; i < sizeof(heights) / sizeof(heights[0]) && count < max - 1; i++) {
		int h = heights[i], w;
		if (h >= max_height)
			break;
		w = (int)lround((double)h * max_width / max_height) & ~1;
		if (w < 16)
			continue;
		rungs[count].width = w;
		rungs[count].height = h;
		count++;
	}
	rungs[count].width = max_width;
	rungs[count].height = max_height;
	return count + 1;
}

static double
rung_kbps(const Adapt *adapt, int rung, double bpp)
{
	return (double)adapt->rungs[rung].width * adapt->rungs[rung].height *
	    adapt->fps * bpp / 1000.0;
}

static double
rung_bpp(const Adapt *adapt, int rung, double kbps)
{
	return kbps * 1000.0 / ((double)adapt->rungs[rung].width *
	    adapt->rungs[rung].height * adapt->fps);
}

/* Highest rung at which kbps gives at least bpp (0 if none). */
static int
best_rung(const Adapt *adapt, double kbps, double bpp)
{
	int r;
	for (r = adapt->count - 1; r > 0; r--)
		if (rung_bpp(adapt, r, kbps) >= bpp)
			return r;
	return 0;
}

double
adapt_video_kbps(const Adapt *adapt, double wire_rate, double ahead)
{
	/* Use less of the link the less media is buffered ahead, so a lead
	 * that shrinks (the encoder overshoots, the link wobbles) refills. */
	double target = adapt->ahead_target > 0.6 ? adapt->ahead_target : 3.0;
	double fill = (ahead - 0.5) / (target - 0.5);
	double use;
	if (fill < 0) fill = 0;
	if (fill > 1) fill = 1;
	use = 0.6 + 0.25 * fill;
	/* ~42 bytes of APC + header per packet, ~700 B/s of pings and the
	 * status line, then base64's 3/4 and the chunk framing. */
	double payload = (wire_rate - adapt->packets_per_second * 42 - 700) *
	    0.75 * 0.998;
	double kbps = payload * 8 / 1000 * use - adapt->audio_kbps;
	return kbps;
}

static double
clamp_kbps(const Adapt *adapt, double kbps)
{
	double cap = rung_kbps(adapt, adapt->rung, ADAPT_BPP_MAX);
	if (cap > adapt->max_kbps)
		cap = adapt->max_kbps;
	if (kbps > cap)
		kbps = cap;
	if (kbps < adapt->min_kbps)
		kbps = adapt->min_kbps;
	return kbps;
}

void
adapt_init(Adapt *adapt, double now, const Rung *rungs, int count,
		double fps, double rate, double audio_kbps, double max_kbps)
{
	double kbps;

	memset(adapt, 0, sizeof(*adapt));
	if (count > ADAPT_RUNGS)
		count = ADAPT_RUNGS;
	memcpy(adapt->rungs, rungs, sizeof(*rungs) * (size_t)count);
	adapt->count = count;
	adapt->fps = fps > 0 ? fps : 30;
	adapt->audio_kbps = audio_kbps;
	adapt->min_kbps = 120;
	adapt->max_kbps = max_kbps > 0 ? max_kbps : 40000;
	adapt->packets_per_second = adapt->fps + (audio_kbps > 0 ? 47 : 0);
	kbps = adapt_video_kbps(adapt, rate, 1.0);
	adapt->usable_kbps = kbps;
	adapt->rung = best_rung(adapt, kbps, ADAPT_BPP_UP);
	adapt->kbps = clamp_kbps(adapt, kbps);
	adapt->last_change = adapt->last_rung_change = now;
	adapt->next_probe = now + 3.0;
}

int
adapt_probing(const Adapt *adapt, double now)
{
	return now < adapt->probe_until;
}

int
adapt_update(Adapt *adapt, Flow *flow, double now, double ahead,
		int app_limited)
{
	double target, bpp;
	int rung = adapt->rung, changed = 0;

	/* Probing: let the window and the media budget grow for a moment so
	 * the sender becomes window-limited and measures the link. */
	if (adapt->probe_until > 0 && now >= adapt->probe_until) {
		adapt->probe_until = 0;
		flow->window_gain = 1.0;
		adapt->next_probe = now + (flow->rate > adapt->probe_start_rate *
		    1.1 ? 2.0 : 6.0);
	}
	if (adapt->fixed)
		return 0;
	target = adapt_video_kbps(adapt, flow->rate, ahead);
	adapt->usable_kbps = target;
	if (!adapt->probe_until && app_limited && now >= adapt->next_probe &&
	    clamp_kbps(adapt, target) < 0.95 * (adapt->max_kbps <
	    rung_kbps(adapt, adapt->count - 1, ADAPT_BPP_MAX) ? adapt->max_kbps :
	    rung_kbps(adapt, adapt->count - 1, ADAPT_BPP_MAX))) {
		adapt->probe_until = now + 1.0;
		adapt->probe_start_rate = flow->rate;
		flow->window_gain = 1.5;
	}

	/* Resolution: up only after it has been affordable for a while,
	 * down quickly, at once if the picture would fall apart. */
	bpp = rung_bpp(adapt, rung, target);
	if (rung + 1 < adapt->count &&
	    rung_bpp(adapt, rung + 1, target) >= ADAPT_BPP_UP) {
		if (!adapt->up_since)
			adapt->up_since = now;
		if (now - adapt->up_since >= ADAPT_UP_HOLD &&
		    now - adapt->last_rung_change >= ADAPT_UP_HOLD)
			rung = best_rung(adapt, target, ADAPT_BPP_UP);
	} else {
		adapt->up_since = 0;
	}
	if (rung == adapt->rung && rung > 0 && bpp < ADAPT_BPP_DOWN) {
		if (!adapt->down_since)
			adapt->down_since = now;
		if (bpp >= ADAPT_BPP_PANIC)
			adapt->panic_since = 0;
		else if (!adapt->panic_since)
			adapt->panic_since = now;
		if ((adapt->panic_since && now - adapt->panic_since >= 0.3) ||
		    now - adapt->down_since >= ADAPT_DOWN_HOLD) {
			rung = best_rung(adapt, target,
			    (ADAPT_BPP_UP + ADAPT_BPP_DOWN) / 2);
			if (rung >= adapt->rung)
				rung = adapt->rung - 1;
		}
	} else if (bpp >= ADAPT_BPP_DOWN) {
		adapt->down_since = adapt->panic_since = 0;
	}
	if (rung != adapt->rung) {
		adapt->rung = rung;
		adapt->up_since = adapt->down_since = adapt->panic_since = 0;
		adapt->last_rung_change = now;
		changed = 1;
	}
	target = clamp_kbps(adapt, target);
	if (changed || fabs(target - adapt->kbps) > 0.08 * adapt->kbps) {
		if (changed || target < adapt->kbps || now - adapt->last_change >= 0.5) {
			adapt->kbps = target;
			adapt->last_change = now;
		}
	}
	return changed;
}

/* macOS st video backend (docs/video.md).
 *
 * VideoToolbox decodes H.264 into IOSurface-backed BGRA pixel buffers, which
 * the Metal renderer samples directly through a CVMetalTextureCache.  AAC
 * goes to an AVSampleBufferAudioRenderer; its AVSampleBufferRenderSynchronizer
 * is the clock every frame is presented against (it runs on the host clock
 * when a video has no sound).  A display link ticks the players while any
 * of them plays, and the terminal redraws only when a shown frame changes. */
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef MIN
#undef MIN
#endif
#ifdef MAX
#undef MAX
#endif

#include "../graphics.h"
#include "../video.h"
#include "renderer.h"
#include "videoplayer.h"

#define TIMESCALE 1000000
/* Frames decoded (or decoding) ahead of the clock.  B-frame reordering
 * needs a few; memory needs a bound (a 1080p BGRA frame is 8 MB). */
#define FRAMES_AHEAD 8
#define FRAMES_MAX (FRAMES_AHEAD * 2)
#define PARAMETER_MAX 1024
/* Audio this close to running out stalls the clock instead of glitching. */
#define AUDIO_DRY_MARGIN 20000
#define VIDEO_DRY_MARGIN 100000

typedef struct VideoQueued {
	struct VideoQueued *next;
	int64_t pts;
	int keyframe;
	size_t length;
	unsigned char data[];
} VideoQueued;

typedef struct {
	VideoQueued *head, *tail;
	size_t bytes;
} VideoQueue;

typedef struct {
	CVPixelBufferRef buffer;
	int64_t pts;
} DecodedFrame;

@interface STVideo : NSObject {
@public
	VideoConfig config;
	/* Video decoding (main thread). */
	VTDecompressionSessionRef session;
	CMVideoFormatDescriptionRef format;
	unsigned char sps[PARAMETER_MAX], pps[PARAMETER_MAX];
	size_t spsLength, ppsLength;
	VideoQueue videoQueue;
	int needKeyframe;
	int64_t frameDuration;
	/* Shared with VideoToolbox's callback thread, under lock. */
	pthread_mutex_t lock;
	DecodedFrame frames[FRAMES_MAX];
	int frameCount;
	int inflight;
	unsigned generation;
	int64_t dropBefore;
	uint64_t dropped;
	const char *decodeError;
	/* The frame on screen. */
	CVPixelBufferRef current;
	int64_t currentPts;
	CVMetalTextureRef texture;
	uint64_t textureSerial;
	int textureStale;
	uint64_t shown;
	int segmentShown;	/* a frame since the open or flush is shown */
	/* Audio. */
	CMAudioFormatDescriptionRef audioFormat;
	VideoQueue audioQueue;
	int64_t audioFrameDuration;
	/* Timeline since the open or the last flush. */
	int paused, ended, started, running, finished;
	int64_t firstPts, videoEnd, audioEnd, lastPosition;
	double volume;
	const char *error;
}
@property(nonatomic, strong) AVSampleBufferRenderSynchronizer *synchronizer;
@property(nonatomic, strong) AVSampleBufferAudioRenderer *audio;
@end

@implementation STVideo
@end

static STVideo *players[VIDEO_MAX_OPEN];
static CVMetalTextureCacheRef textureCache;
static id<MTLDevice> metalDevice;
static NSView *linkView;
static id displayLink;
static NSTimer *fallbackTimer;
static void (*requestRedraw)(void);
/* ST_VIDEO_VOLUME (0..1) scales every video's volume: tests and harnesses
 * run real playback silently with ST_VIDEO_VOLUME=0. */
static double volumeScale = 1;

static void updateTicking(void);
static void backend_status(VideoPlayer *handle, VideoStatus *status);

static int64_t
cmtime_us(CMTime time)
{
	if (!CMTIME_IS_NUMERIC(time))
		return VIDEO_TIME_NONE;
	return CMTimeConvertScale(time, TIMESCALE,
	    kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
}

static void
queue_push(VideoQueue *queue, VideoQueued *item)
{
	item->next = NULL;
	if (queue->tail)
		queue->tail->next = item;
	else
		queue->head = item;
	queue->tail = item;
	queue->bytes += item->length;
}

static VideoQueued *
queue_pop(VideoQueue *queue)
{
	VideoQueued *item = queue->head;
	if (!item)
		return NULL;
	queue->head = item->next;
	if (!queue->head)
		queue->tail = NULL;
	queue->bytes -= item->length;
	return item;
}

static void
queue_clear(VideoQueue *queue)
{
	VideoQueued *item;
	while ((item = queue_pop(queue)))
		free(item);
}

static void
set_error(STVideo *player, const char *error)
{
	if (!player->error)
		player->error = error;
}

/* Frames: the VideoToolbox callback inserts by pts, the main thread takes
 * them in order.  Caller holds the lock. */
static void
frames_clear_locked(STVideo *player)
{
	for (int i = 0; i < player->frameCount; i++)
		CVPixelBufferRelease(player->frames[i].buffer);
	player->frameCount = 0;
}

static void
decoded(void *refcon, void *frameRefcon, OSStatus status,
		VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts,
		CMTime duration)
{
	STVideo *player = (__bridge STVideo *)refcon;
	unsigned generation = (unsigned)(uintptr_t)frameRefcon;
	int64_t time = cmtime_us(pts);
	int wake = 0;

	(void)duration;
	pthread_mutex_lock(&player->lock);
	if (player->inflight > 0)
		player->inflight--;
	if (generation != player->generation) {
		pthread_mutex_unlock(&player->lock);
		return;
	}
	if (status != noErr || !image || (flags & kVTDecodeInfo_FrameDropped)) {
		if (status != noErr && !player->decodeError)
			player->decodeError = "EBADMSG:video decode failed";
	} else if (time == VIDEO_TIME_NONE ||
	    (player->dropBefore != VIDEO_TIME_NONE && time < player->dropBefore) ||
	    player->frameCount == FRAMES_MAX) {
		player->dropped++;
	} else {
		int i = player->frameCount;
		while (i > 0 && player->frames[i - 1].pts > time) {
			player->frames[i] = player->frames[i - 1];
			i--;
		}
		player->frames[i] = (DecodedFrame){CVPixelBufferRetain(image), time};
		player->frameCount++;
		wake = 1;
	}
	pthread_mutex_unlock(&player->lock);
	if (wake)
		dispatch_async(dispatch_get_main_queue(), ^{
			updateTicking();
		});
}

static void
destroy_session(STVideo *player)
{
	if (!player->session)
		return;
	VTDecompressionSessionWaitForAsynchronousFrames(player->session);
	VTDecompressionSessionInvalidate(player->session);
	CFRelease(player->session);
	player->session = NULL;
}

static int
make_session(STVideo *player, CMVideoFormatDescriptionRef format)
{
	NSDictionary *attributes = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
		(id)kCVPixelBufferMetalCompatibilityKey: @YES,
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
	};
	VTDecompressionOutputCallbackRecord callback = {
		decoded, (__bridge void *)player
	};
	VTDecompressionSessionRef session = NULL;
	OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault,
	    format, NULL, (__bridge CFDictionaryRef)attributes, &callback,
	    &session);
	if (status != noErr || !session)
		return 0;
	player->session = session;
	return 1;
}

/* New parameter sets: keep the session when it can decode them (same
 * profile/size limits), otherwise drain it and start another. */
static int
update_format(STVideo *player)
{
	const uint8_t *sets[2] = {player->sps, player->pps};
	size_t sizes[2] = {player->spsLength, player->ppsLength};
	CMVideoFormatDescriptionRef format = NULL;

	if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
	    2, sets, sizes, 4, &format) != noErr || !format)
		return 0;
	if (player->session &&
	    !VTDecompressionSessionCanAcceptFormatDescription(player->session,
	    format))
		destroy_session(player);
	if (!player->session && !make_session(player, format)) {
		CFRelease(format);
		return 0;
	}
	if (player->format)
		CFRelease(player->format);
	player->format = format;
	return 1;
}

static const unsigned char *
next_nal(const unsigned char *p, const unsigned char *end, size_t *length)
{
	const unsigned char *start = NULL, *q;

	for (q = p; q + 3 <= end; q++)
		if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
			start = q + 3;
			break;
		}
	if (!start)
		return NULL;
	for (q = start; q + 3 <= end; q++)
		if (q[0] == 0 && q[1] == 0 && (q[2] == 1 ||
		    (q[2] == 0 && q + 3 < end && q[3] == 1)))
			break;
	if (q + 3 > end)
		q = end;
	*length = (size_t)(q - start);
	return start;
}

/* Annex B access unit -> parameter sets + a length-prefixed sample, then an
 * asynchronous decode. */
static void
decode_packet(STVideo *player, VideoQueued *item)
{
	const unsigned char *p = item->data, *end = item->data + item->length;
	const unsigned char *nal;
	size_t length, used = 0;
	unsigned char *avcc;
	int changed = 0;

	avcc = malloc(item->length + 64);
	if (!avcc)
		return;
	while ((nal = next_nal(p, end, &length))) {
		p = nal + length;
		/* Trailing zero bytes belong to the next start code. */
		while (length && nal[length - 1] == 0)
			length--;
		if (!length)
			continue;
		switch (nal[0] & 0x1f) {
		case 7:
			if (length <= PARAMETER_MAX && (length != player->spsLength ||
			    memcmp(nal, player->sps, length))) {
				memcpy(player->sps, nal, length);
				player->spsLength = length;
				changed = 1;
			}
			break;
		case 8:
			if (length <= PARAMETER_MAX && (length != player->ppsLength ||
			    memcmp(nal, player->pps, length))) {
				memcpy(player->pps, nal, length);
				player->ppsLength = length;
				changed = 1;
			}
			break;
		case 9:	/* access unit delimiter */
			break;
		default:
			if (used + 4 + length > item->length + 64)
				break;
			avcc[used++] = (unsigned char)(length >> 24);
			avcc[used++] = (unsigned char)(length >> 16);
			avcc[used++] = (unsigned char)(length >> 8);
			avcc[used++] = (unsigned char)length;
			memcpy(avcc + used, nal, length);
			used += length;
			break;
		}
	}
	if (changed && player->spsLength && player->ppsLength &&
	    !update_format(player))
		set_error(player, "EBADMSG:invalid H.264 parameter sets");
	if (!player->session || !used ||
	    (player->needKeyframe && !item->keyframe)) {
		free(avcc);
		return;
	}
	player->needKeyframe = 0;

	CMBlockBufferRef block = NULL;
	CMSampleBufferRef sample = NULL;
	if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, avcc, used,
	    kCFAllocatorMalloc, NULL, 0, used, 0, &block) != noErr) {
		free(avcc);
		return;
	}
	CMSampleTimingInfo timing = {
		.duration = kCMTimeInvalid,
		.presentationTimeStamp = CMTimeMake(item->pts, TIMESCALE),
		.decodeTimeStamp = kCMTimeInvalid,
	};
	if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, player->format,
	    1, 1, &timing, 1, &used, &sample) != noErr) {
		CFRelease(block);
		return;
	}
	CFRelease(block);
	pthread_mutex_lock(&player->lock);
	player->inflight++;
	pthread_mutex_unlock(&player->lock);
	VTDecodeInfoFlags info = 0;
	OSStatus status = VTDecompressionSessionDecodeFrame(player->session,
	    sample, kVTDecodeFrame_EnableAsynchronousDecompression,
	    (void *)(uintptr_t)player->generation, &info);
	CFRelease(sample);
	if (status != noErr) {
		/* A frame VideoToolbox refused never reaches the callback. */
		pthread_mutex_lock(&player->lock);
		if (player->inflight > 0)
			player->inflight--;
		pthread_mutex_unlock(&player->lock);
		if (status != kVTInvalidSessionErr)
			set_error(player, "EBADMSG:video decode failed");
	}
}

static void
feed_decoder(STVideo *player)
{
	VideoQueued *item;

	for (;;) {
		pthread_mutex_lock(&player->lock);
		int ahead = player->frameCount + player->inflight;
		pthread_mutex_unlock(&player->lock);
		if (ahead >= FRAMES_AHEAD || !(item = queue_pop(&player->videoQueue)))
			return;
		decode_packet(player, item);
		free(item);
	}
}

/* The AAC "magic cookie" Core Audio decoders expect: an MPEG-4 ES
 * descriptor wrapping the AudioSpecificConfig (as in an MP4 esds box). */
static size_t
esds_cookie(const unsigned char *config, size_t length, unsigned char *out)
{
	unsigned char *p = out;
#define DESCRIPTOR(tag, size) do { *p++ = (tag); *p++ = 0x80; *p++ = 0x80; \
	*p++ = 0x80; *p++ = (unsigned char)(size); } while (0)
	DESCRIPTOR(0x03, 3 + 5 + 13 + 5 + length);
	*p++ = 0; *p++ = 0;	/* ES_ID */
	*p++ = 0;		/* flags */
	DESCRIPTOR(0x04, 13 + 5 + length);
	*p++ = 0x40;		/* MPEG-4 audio */
	*p++ = 0x15;		/* audio stream */
	memset(p, 0, 11);	/* buffer size, max and average bitrate */
	p += 11;
	DESCRIPTOR(0x05, length);
	memcpy(p, config, length);
	p += length;
#undef DESCRIPTOR
	return (size_t)(p - out);
}

static int
make_audio(STVideo *player)
{
	AudioStreamBasicDescription asbd = {
		.mSampleRate = player->config.sample_rate,
		.mFormatID = kAudioFormatMPEG4AAC,
		.mFormatFlags = kMPEG4Object_AAC_LC,
		.mFramesPerPacket = 1024,
		.mChannelsPerFrame = (UInt32)player->config.channels,
	};
	AudioChannelLayout layout = {
		.mChannelLayoutTag = player->config.channels == 1 ?
		    kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo,
	};
	unsigned char cookie[VIDEO_AUDIO_CONFIG_MAX + 64];
	size_t cookieLength = esds_cookie(player->config.audio_config,
	    player->config.audio_config_length, cookie);

	if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd,
	    sizeof(layout), &layout, cookieLength, cookie, NULL,
	    &player->audioFormat) != noErr)
		return 0;
	player.audio = [AVSampleBufferAudioRenderer new];
	player.audio.volume = (float)(player->volume * volumeScale);
	[player.synchronizer addRenderer:player.audio];
	player->audioFrameDuration = 1024LL * TIMESCALE / player->config.sample_rate;
	return 1;
}

static void
feed_audio(STVideo *player)
{
	VideoQueued *item;

	while (player.audio && player.audio.isReadyForMoreMediaData &&
	    (item = queue_pop(&player->audioQueue))) {
		CMBlockBufferRef block = NULL;
		CMSampleBufferRef sample = NULL;
		AudioStreamPacketDescription description = {
			0, 0, (UInt32)item->length
		};
		if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL,
		    item->length, kCFAllocatorDefault, NULL, 0, item->length,
		    kCMBlockBufferAssureMemoryNowFlag, &block) == noErr &&
		    CMBlockBufferReplaceDataBytes(item->data, block, 0,
		    item->length) == noErr &&
		    CMAudioSampleBufferCreateReadyWithPacketDescriptions(
		    kCFAllocatorDefault, block, player->audioFormat, 1,
		    CMTimeMake(item->pts, TIMESCALE), &description,
		    &sample) == noErr)
			[player.audio enqueueSampleBuffer:sample];
		if (sample)
			CFRelease(sample);
		if (block)
			CFRelease(block);
		free(item);
	}
	if (player.audio.status == AVQueuedSampleBufferRenderingStatusFailed)
		set_error(player, "EBADMSG:audio decode failed");
}

static int64_t
clock_now(STVideo *player)
{
	return player->started ? cmtime_us(player.synchronizer.currentTime) :
	    VIDEO_TIME_NONE;
}

static int64_t
media_end(STVideo *player)
{
	int64_t end = VIDEO_TIME_NONE;
	if (player->config.video_codec && player->videoEnd != VIDEO_TIME_NONE)
		end = player->videoEnd + player->frameDuration;
	if (player->config.audio_codec && player->audioEnd != VIDEO_TIME_NONE) {
		int64_t audio = player->audioEnd + player->audioFrameDuration;
		end = end == VIDEO_TIME_NONE || audio > end ? audio : end;
	}
	return end;
}

/* Media queued beyond `from`, limited by the stream that has least. */
static int64_t
buffered_ahead(STVideo *player, int64_t from)
{
	int64_t ahead = INT64_MAX;
	if (from == VIDEO_TIME_NONE)
		return 0;
	if (player->config.video_codec) {
		if (player->videoEnd == VIDEO_TIME_NONE)
			return 0;
		ahead = player->videoEnd + player->frameDuration - from;
	}
	if (player->config.audio_codec) {
		int64_t audio;
		if (player->audioEnd == VIDEO_TIME_NONE)
			return 0;
		audio = player->audioEnd + player->audioFrameDuration - from;
		if (audio < ahead)
			ahead = audio;
	}
	return ahead;
}

static int
has_frames(STVideo *player)
{
	pthread_mutex_lock(&player->lock);
	int count = player->frameCount;
	pthread_mutex_unlock(&player->lock);
	/* A frame from before a flush doesn't count. */
	return count > 0 || player->segmentShown;
}

static void
set_rate(STVideo *player, float rate)
{
	if (player.synchronizer.rate != rate)
		player.synchronizer.rate = rate;
}

/* buffering -> playing -> buffering/ended, per docs/video.md. */
static void
advance_state(STVideo *player)
{
	int64_t now;

	if (player->paused || player->finished)
		return;
	if (!player->running) {
		int64_t from = player->started ? clock_now(player) :
		    player->firstPts;
		int ready = player->ended ? (player->firstPts != VIDEO_TIME_NONE ||
		    player->started) :
		    buffered_ahead(player, from) >=
		    (int64_t)player->config.prebuffer_ms * 1000;
		if (player->config.video_codec && !has_frames(player) &&
		    player->videoQueue.head)
			ready = 0;
		if (!ready || from == VIDEO_TIME_NONE)
			return;
		if (!player->started) {
			[player.synchronizer setRate:1.0
			    time:CMTimeMake(player->firstPts, TIMESCALE)];
			player->started = 1;
		} else {
			set_rate(player, 1.0);
		}
		player->running = 1;
		return;
	}
	now = clock_now(player);
	if (player->ended) {
		int64_t end = media_end(player);
		if (!player->videoQueue.head && !player->audioQueue.head &&
		    (end == VIDEO_TIME_NONE || now >= end)) {
			set_rate(player, 0);
			player->lastPosition = now;
			player->running = 0;
			player->finished = 1;
		}
		return;
	}
	if ((player->config.audio_codec && (player->audioEnd == VIDEO_TIME_NONE ||
	    now >= player->audioEnd + player->audioFrameDuration -
	    AUDIO_DRY_MARGIN)) ||
	    (player->config.video_codec && (player->videoEnd == VIDEO_TIME_NONE ||
	    now > player->videoEnd + VIDEO_DRY_MARGIN))) {
		set_rate(player, 0);
		player->running = 0;
	}
}

/* Pick the newest decoded frame due by media time `due`.  Returns 1 if the
 * frame to show changed. */
static int
select_frame(STVideo *player, int64_t due)
{
	CVPixelBufferRef chosen = NULL;
	int64_t pts = 0;
	int index = -1;

	pthread_mutex_lock(&player->lock);
	if (player->started)
		for (int i = 0; i < player->frameCount; i++)
			if (player->frames[i].pts <= due)
				index = i;
	/* After an open or seek, show the new segment's first frame at once,
	 * even while it buffers or if it is due a moment after the clock. */
	if (index < 0 && !player->segmentShown && player->frameCount)
		index = 0;
	if (index >= 0) {
		player->dropped += (uint64_t)index;
		for (int i = 0; i < index; i++)
			CVPixelBufferRelease(player->frames[i].buffer);
		chosen = player->frames[index].buffer;
		pts = player->frames[index].pts;
		player->frameCount -= index + 1;
		memmove(player->frames, player->frames + index + 1,
		    (size_t)player->frameCount * sizeof(*player->frames));
	}
	pthread_mutex_unlock(&player->lock);
	if (!chosen)
		return 0;
	if (player->current)
		CVPixelBufferRelease(player->current);
	player->current = chosen;
	player->currentPts = pts;
	player->textureStale = 1;
	player->segmentShown = 1;
	player->shown++;
	return 1;
}

static int
tick_player(STVideo *player, double target)
{
	const char *error;
	int64_t due;

	pthread_mutex_lock(&player->lock);
	error = player->decodeError;
	pthread_mutex_unlock(&player->lock);
	if (error)
		set_error(player, error);
	feed_audio(player);
	feed_decoder(player);
	advance_state(player);
	due = clock_now(player);
	if (due != VIDEO_TIME_NONE && player->running)
		due += (int64_t)((target - CACurrentMediaTime()) * TIMESCALE);
	return select_frame(player, due);
}

static int
needs_ticks(STVideo *player)
{
	return player && !player->finished && !player->paused;
}

size_t
mac_video_describe(char *out, size_t size)
{
	size_t used = 0;

	if (size)
		out[0] = '\0';
	for (int i = 0; i < VIDEO_MAX_OPEN && used < size; i++) {
		VideoStatus status;
		if (!players[i])
			continue;
		backend_status((__bridge VideoPlayer *)players[i], &status);
		int n = snprintf(out + used, size - used,
		    "%s%s pos=%.2f vbuf=%.2f abuf=%.2f shown=%llu dropped=%llu%s%s",
		    used ? "; " : "", video_state_name(status.state),
		    status.position == VIDEO_TIME_NONE ? -1 : status.position / 1e6,
		    status.video_end == VIDEO_TIME_NONE ? -1 : status.video_end / 1e6,
		    status.audio_end == VIDEO_TIME_NONE ? -1 : status.audio_end / 1e6,
		    (unsigned long long)status.shown,
		    (unsigned long long)status.dropped,
		    status.error ? " error=" : "", status.error ? status.error : "");
		if (n < 0)
			break;
		used += (size_t)n;
	}
	return used < size ? used : size;
}

int
mac_video_active(void)
{
	for (int i = 0; i < VIDEO_MAX_OPEN; i++)
		if (needs_ticks(players[i]))
			return 1;
	return 0;
}

int
mac_video_tick(double target)
{
	int changed = 0;

	for (int i = 0; i < VIDEO_MAX_OPEN; i++)
		if (players[i])
			changed |= tick_player(players[i], target);
	if (textureCache)
		CVMetalTextureCacheFlush(textureCache, 0);
	if (changed && requestRedraw)
		requestRedraw();
	return changed;
}

@interface STVideoTicker : NSObject
@end

@implementation STVideoTicker
- (void)displayLinkTick:(id)link
{
	double target = CACurrentMediaTime();
	if (@available(macOS 14.0, *))
		target = ((CADisplayLink *)link).targetTimestamp;
	mac_video_tick(target);
	updateTicking();
}

- (void)timerTick:(NSTimer *)timer
{
	mac_video_tick(CACurrentMediaTime() + MIN(timer.timeInterval, 1.0 / 60));
	updateTicking();
}
@end

static STVideoTicker *ticker;

/* Ticks run only while a video plays or waits for data, so an idle
 * terminal (or a paused or finished video) does no work.  The display link
 * paces frames; a slow timer beside it keeps buffering, end of stream and
 * the decoder going when the link stops firing (the window hidden or on
 * another space).  Without a display link (before macOS 14) the timer runs
 * at frame rate. */
static void
updateTicking(void)
{
	int active = mac_video_active(), linked = 0;
	double interval;

	if (!ticker)
		return;
	if (linkView) {
		if (@available(macOS 14.0, *)) {
			if (!displayLink && active) {
				CADisplayLink *link = [linkView displayLinkWithTarget:ticker
				    selector:@selector(displayLinkTick:)];
				[link addToRunLoop:NSRunLoop.mainRunLoop
				    forMode:NSRunLoopCommonModes];
				displayLink = link;
			}
			if (displayLink)
				((CADisplayLink *)displayLink).paused = !active;
			linked = displayLink != nil;
		}
	}
	interval = linked ? 0.1 : 1.0 / 120;
	if (fallbackTimer && (!active || fallbackTimer.timeInterval != interval)) {
		[fallbackTimer invalidate];
		fallbackTimer = nil;
	}
	if (active && !fallbackTimer) {
		fallbackTimer = [NSTimer timerWithTimeInterval:interval
		    target:ticker selector:@selector(timerTick:) userInfo:nil
		    repeats:YES];
		[NSRunLoop.mainRunLoop addTimer:fallbackTimer
		    forMode:NSRunLoopCommonModes];
	}
}

int
mac_video_bind(VideoPlayer *handle, uint64_t serial)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;
	CVMetalTextureRef texture = NULL;
	size_t width, height;

	if (!player || !player->current || !textureCache)
		return 0;
	if (!player->textureStale && player->texture &&
	    player->textureSerial == serial)
		return 1;
	width = CVPixelBufferGetWidth(player->current);
	height = CVPixelBufferGetHeight(player->current);
	if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
	    textureCache, player->current, NULL, MTLPixelFormatBGRA8Unorm,
	    width, height, 0, &texture) != kCVReturnSuccess || !texture)
		return player->texture != NULL;
	mac_renderer_set_image_texture(serial,
	    (__bridge void *)CVMetalTextureGetTexture(texture));
	/* The previous frame may still be on the GPU: keep it (and the pixel
	 * buffer behind it) until the frame being built has completed. */
	if (player->texture)
		mac_renderer_release_after_frame(player->texture);
	mac_renderer_release_after_frame(CVPixelBufferRetain(player->current));
	player->texture = texture;
	player->textureSerial = serial;
	player->textureStale = 0;
	return 1;
}

static void
reset_timeline(STVideo *player)
{
	player->started = player->running = player->finished = 0;
	player->ended = player->segmentShown = 0;
	player->firstPts = player->videoEnd = player->audioEnd = VIDEO_TIME_NONE;
	player->lastPosition = VIDEO_TIME_NONE;
}

static VideoPlayer *
backend_create(const VideoConfig *config, const char **error)
{
	int slot;

	for (slot = 0; slot < VIDEO_MAX_OPEN && players[slot]; slot++)
		;
	if (slot == VIDEO_MAX_OPEN) {
		*error = "ENOSPC:too many open videos";
		return NULL;
	}
	STVideo *player = [STVideo new];
	player->config = *config;
	pthread_mutex_init(&player->lock, NULL);
	player->dropBefore = VIDEO_TIME_NONE;
	player->needKeyframe = 1;
	player->frameDuration = 1000000 / 30;
	player->paused = config->paused;
	player->volume = config->volume;
	reset_timeline(player);
	player.synchronizer = [AVSampleBufferRenderSynchronizer new];
	if (config->audio_codec && !make_audio(player)) {
		pthread_mutex_destroy(&player->lock);
		*error = "ENOTSUP:audio format not supported";
		return NULL;
	}
	players[slot] = player;
	updateTicking();
	return (__bridge_retained VideoPlayer *)player;
}

static void
backend_destroy(VideoPlayer *handle)
{
	STVideo *player = (__bridge_transfer STVideo *)(void *)handle;

	for (int i = 0; i < VIDEO_MAX_OPEN; i++)
		if (players[i] == player)
			players[i] = nil;
	set_rate(player, 0);
	if (player.audio) {
		[player.audio flush];
		[player.synchronizer removeRenderer:player.audio
		    atTime:kCMTimeInvalid completionHandler:nil];
	}
	destroy_session(player);
	pthread_mutex_lock(&player->lock);
	frames_clear_locked(player);
	pthread_mutex_unlock(&player->lock);
	queue_clear(&player->videoQueue);
	queue_clear(&player->audioQueue);
	if (player->current)
		CVPixelBufferRelease(player->current);
	/* The renderer dropped the texture with the image; the GPU may still
	 * be reading it. */
	if (player->texture)
		mac_renderer_release_after_frame(player->texture);
	if (player->format)
		CFRelease(player->format);
	if (player->audioFormat)
		CFRelease(player->audioFormat);
	pthread_mutex_destroy(&player->lock);
	player.audio = nil;
	player.synchronizer = nil;
	updateTicking();
}

static int
backend_packet(VideoPlayer *handle, const VideoPacket *packet,
		const char **error)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;
	VideoQueue *queue;
	VideoQueued *item;
	int video = packet->stream == VIDEO_STREAM_VIDEO;

	if (video ? !player->config.video_codec : !player->config.audio_codec) {
		*error = "EINVAL:packet for a stream the video does not have";
		return 0;
	}
	if (player->videoQueue.bytes + player->audioQueue.bytes +
	    packet->length > VIDEO_QUEUE_MAX) {
		*error = "ENOBUFS:video queue is full";
		return 0;
	}
	if (!video && player->dropBefore != VIDEO_TIME_NONE &&
	    packet->pts + player->audioFrameDuration <= player->dropBefore)
		return 1;
	item = malloc(sizeof(*item) + packet->length);
	if (!item) {
		*error = "ENOMEM:video packet allocation failed";
		return 0;
	}
	item->pts = packet->pts;
	item->keyframe = packet->keyframe;
	item->length = packet->length;
	memcpy(item->data, packet->data, packet->length);
	queue = video ? &player->videoQueue : &player->audioQueue;
	queue_push(queue, item);
	if (video) {
		if (player->videoEnd != VIDEO_TIME_NONE &&
		    packet->pts > player->videoEnd &&
		    packet->pts - player->videoEnd < 1000000)
			player->frameDuration = packet->pts - player->videoEnd;
		if (player->videoEnd == VIDEO_TIME_NONE ||
		    packet->pts > player->videoEnd)
			player->videoEnd = packet->pts;
	} else if (player->audioEnd == VIDEO_TIME_NONE ||
	    packet->pts > player->audioEnd) {
		player->audioEnd = packet->pts;
	}
	{
		int64_t first = packet->pts;
		if (player->dropBefore != VIDEO_TIME_NONE &&
		    first < player->dropBefore)
			first = player->dropBefore;
		if (player->firstPts == VIDEO_TIME_NONE || first < player->firstPts)
			player->firstPts = first;
	}
	if (video)
		feed_decoder(player);
	else
		feed_audio(player);
	updateTicking();
	return 1;
}

static void
backend_flush(VideoPlayer *handle, int64_t start)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;

	set_rate(player, 0);
	if (player.audio)
		[player.audio flush];
	queue_clear(&player->videoQueue);
	queue_clear(&player->audioQueue);
	pthread_mutex_lock(&player->lock);
	player->generation++;
	player->dropBefore = start;
	player->decodeError = NULL;
	frames_clear_locked(player);
	pthread_mutex_unlock(&player->lock);
	if (player->session)
		VTDecompressionSessionWaitForAsynchronousFrames(player->session);
	player->needKeyframe = 1;
	player->error = NULL;
	/* The current frame stays on screen until the new segment's first. */
	reset_timeline(player);
	updateTicking();
}

static void
backend_end(VideoPlayer *handle)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;
	player->ended = 1;
	updateTicking();
}

static void
backend_pause(VideoPlayer *handle, int paused)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;

	if (player->paused == !!paused)
		return;
	player->paused = !!paused;
	if (paused) {
		set_rate(player, 0);
		player->running = 0;
	}
	updateTicking();
}

static void
backend_volume(VideoPlayer *handle, double volume)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;
	player->volume = volume;
	if (player.audio)
		player.audio.volume = (float)(volume * volumeScale);
}

static void
backend_status(VideoPlayer *handle, VideoStatus *status)
{
	STVideo *player = (__bridge STVideo *)(void *)handle;
	const char *error;

	pthread_mutex_lock(&player->lock);
	error = player->decodeError;
	status->dropped = player->dropped;
	pthread_mutex_unlock(&player->lock);
	if (error)
		set_error(player, error);
	status->state = player->finished ? VIDEO_STATE_ENDED :
	    player->paused ? VIDEO_STATE_PAUSED :
	    player->running ? VIDEO_STATE_PLAYING : VIDEO_STATE_BUFFERING;
	status->position = player->finished ? player->lastPosition :
	    clock_now(player);
	status->video_end = player->videoEnd;
	status->audio_end = player->audioEnd;
	status->shown = player->shown;
	status->error = player->error;
}

static const VideoBackend backend = {
	"video=h264 audio=aac max=4096x4096",
	backend_create, backend_destroy, backend_packet, backend_flush,
	backend_end, backend_pause, backend_volume, backend_status,
};

void
mac_video_init(void *device, void *view, void (*redraw)(void))
{
	const char *scale = getenv("ST_VIDEO_VOLUME");

	if (scale && *scale)
		volumeScale = MAX(0.0, MIN(1.0, atof(scale)));
	metalDevice = (__bridge id<MTLDevice>)device;
	if (!textureCache && CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL,
	    metalDevice, NULL, &textureCache) != kCVReturnSuccess)
		return;
	linkView = (__bridge NSView *)view;
	requestRedraw = redraw;
	if (!ticker)
		ticker = [STVideoTicker new];
	graphics_set_video_backend(&backend);
}

void
mac_video_shutdown(void)
{
	graphics_set_video_backend(NULL);
	if (displayLink) {
		if (@available(macOS 14.0, *))
			[(CADisplayLink *)displayLink invalidate];
		displayLink = nil;
	}
	[fallbackTimer invalidate];
	fallbackTimer = nil;
}

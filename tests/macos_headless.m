/* A windowless st for end-to-end checks on macOS: runs a command in a PTY
 * through st's real terminal core, Kitty graphics and video players, renders
 * frames with the real Metal renderer offscreen and saves PNG snapshots.
 *
 *   tests/macos_headless -c 100 -r 30 -t 20 -s 2 -o /tmp/shots -- \
 *       ssh -t whale st-play clip.mp4
 *
 * Sound plays unless ST_VIDEO_VOLUME=0 (the harness sets it unless -a).
 * Each snapshot prints the video players' states.  Not part of `make test`.
 *
 * -A runs the app's own loop instead (see appMain below) and measures it:
 *
 *   tests/macos_headless -A -g 1512x940 -t 30 -p 0:start -p 8:scroll \
 *       -K 8:14:30:j -p 15:type -K 15:20:8:mark -- bun run src/main.ts --demo
 *
 * prints, per phase, frame times, main-thread stalls, key-to-screen latency
 * (keys named "mark" type U+00A4 and count until it is on a shown frame),
 * PTY bytes per second and CPU use.  ST_DISPLAYLINK_THROTTLE_MS=100 slows
 * the emulated display link (not the display) to one frame per 100 ms, as
 * macOS once did to a window; the glide watchdog should keep glides on time. */
#define main st_backend_main
#include "../macos/backend.m"
#undef main
#include "../macos/renderer.m"
#import <ImageIO/ImageIO.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>

static id<MTLTexture> headlessTarget;
static int headlessFrames;

/* -k SECONDS:KEY: type KEY (a character, or space/left/right/up/down/
 * esc/enter) into the terminal at SECONDS, as if pressed. */
typedef struct {
	double at;
	char bytes[8];
	size_t length;
	int sent;
} HeadlessKey;
static HeadlessKey keys[4096];
static int keyCount;

static int
parseKey(const char *spec)
{
	static const struct { const char *name, *bytes; } named[] = {
		{"space", " "}, {"left", "\033[D"}, {"right", "\033[C"},
		{"up", "\033[A"}, {"down", "\033[B"}, {"esc", "\033"},
		{"enter", "\r"}, {"ctrl-d", "\004"}, {"ctrl-u", "\025"},
		{"ctrl-e", "\005"}, {"ctrl-y", "\031"}, {"ctrl-n", "\016"},
		{"ctrl-j", "\n"}, {"ctrl-k", "\013"},
		{"mark", "\302\244"},
	};
	const char *colon = strchr(spec, ':');
	HeadlessKey *key;

	if (!colon || keyCount == (int)(sizeof(keys) / sizeof(keys[0])))
		return 0;
	key = &keys[keyCount];
	key->at = atof(spec);
	for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
		if (!strcmp(colon + 1, named[i].name)) {
			key->length = strlen(named[i].bytes);
			memcpy(key->bytes, named[i].bytes, key->length);
		}
	if (!key->length) {
		key->length = MIN(strlen(colon + 1), sizeof(key->bytes));
		memcpy(key->bytes, colon + 1, key->length);
	}
	keyCount++;
	return 1;
}

/* -K START:END:HZ:KEY: KEY repeated HZ times a second from START to END
 * seconds, like a held key's autorepeat. */
static int
parseRepeat(const char *spec)
{
	double begin, end, hz;
	char name[32], one[64];

	if (sscanf(spec, "%lf:%lf:%lf:%31s", &begin, &end, &hz, name) != 4 ||
	    hz <= 0 || end < begin)
		return 0;
	for (double at = begin; at < end; at += 1.0 / hz) {
		snprintf(one, sizeof(one), "%.4f:%s", at, name);
		if (!parseKey(one))
			return 0;
	}
	return 1;
}

static void
headlessRedraw(void)
{
}

static void
releaseFrameObjects(void)
{
	for (size_t i = 0; i < r.releaseCount; i++)
		CFRelease(r.releases[i]);
	r.releaseCount = 0;
}

/* draw() without a drawable: the grid, then images and videos. */
static void
renderTerminal(void)
{
	MacColor clear = indexedColor(IS_SET(MODE_REVERSE) ? defaultfg : defaultbg);
	for (int i = 0; i < MAC_LAYER_COUNT; i++)
		listreset(&r.layers[i]);
	for (int i = 0; i < 3; i++)
		r.imageLayers[i].count = 0;
	image_occlusion_begin(&imageOcclusion);
	for (int y = 0; y < trow(); y++)
		xdrawline(tlineviewline(y), 0, y, tcol());
	tlineviewprepare();
	mac_renderer_set_image_clip(borderpx, borderpx, win.tw, win.th);
	for (int stage = 0; stage < 3; stage++)
		graphics_draw_rows(tisaltscreen(), stage, win.cw, win.ch, 0, trow(),
		    tlineviewrow, drawGraphicsPlacement, (void *)(intptr_t)stage);
	image_occlusion_end(&imageOcclusion);

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
	pass.colorAttachments[0].texture = headlessTarget;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor =
	    MTLClearColorMake(clear.r, clear.g, clear.b, 1);
	id<MTLCommandBuffer> command = [r.queue commandBuffer];
	id<MTLRenderCommandEncoder> encoder =
	    [command renderCommandEncoderWithDescriptor:pass];
	encodeFrame(encoder);
	[encoder endEncoding];
	[command commit];
	[command waitUntilCompleted];
	releaseFrameObjects();
	headlessFrames++;
}

static void
saveTexture(id<MTLTexture> texture, const char *path)
{
	size_t width = texture.width, height = texture.height;
	unsigned char *pixels = malloc(width * height * 4);
	[texture getBytes:pixels bytesPerRow:width * 4
	    fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8,
	    width * 4, space,
	    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGImageRef image = CGBitmapContextCreateImage(context);
	NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
	CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
	    (__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
	CGImageDestinationAddImage(destination, image, NULL);
	CGImageDestinationFinalize(destination);
	CFRelease(destination);
	CGImageRelease(image);
	CGContextRelease(context);
	CGColorSpaceRelease(space);
	free(pixels);
}

static void
saveSnapshot(const char *path)
{
	saveTexture(headlessTarget, path);
}

@interface HeadlessView : STMetalView
- (void)headlessDraw;
@end

/* ---- -A: the app's own loop, measured ------------------------------------
 *
 * The backend's tty source and redraw requests run unchanged.  The display
 * is emulated: a view marked for display draws when the run loop is about to
 * wait (as AppKit displays it), an unpaused view draws at every vblank, and
 * frames render into three textures that, like a CAMetalLayer's drawables,
 * only come back once a newer frame has been shown at a vblank, so drawing
 * ahead of the display blocks as nextDrawable does. */

typedef struct {
	double *items;
	size_t count, capacity;
} Samples;

static void
samplesAdd(Samples *list, double value)
{
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 256;
		double *grown = realloc(list->items, capacity * sizeof(*grown));
		if (!grown)
			return;
		list->items = grown;
		list->capacity = capacity;
	}
	list->items[list->count++] = value;
}

static int
compareDoubles(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* Nearest-rank percentile; sorts the list. */
static double
samplesPercentile(Samples *list, double p)
{
	size_t rank;

	if (!list->count)
		return 0;
	qsort(list->items, list->count, sizeof(*list->items), compareDoubles);
	rank = (size_t)ceil(p / 100 * list->count);
	return list->items[rank ? rank - 1 : 0];
}

enum { SLOT_FREE, SLOT_DRAWING, SLOT_DONE, SLOT_SHOWN };

typedef struct {
	__strong id<MTLTexture> texture;
	int state, held, releaseWhenUnheld;
	unsigned long sequence;
	int moving, marks;
} DisplaySlot;

#define DISPLAY_SLOTS 3

static struct {
	HeadlessView *view;
	DisplaySlot slots[DISPLAY_SLOTS];
	DisplaySlot *drawing, *shown;
	pthread_mutex_t lock;
	dispatch_semaphore_t free;
	dispatch_queue_t vblankQueue;
	dispatch_source_t vblank, heartbeat;
	double period, lastPresent, lastBeat;
	unsigned long sequence;
	int lastMoving, vblankDrawQueued, needsDisplay;
	/* key-to-screen latency */
	double markSent[4096];
	int marksSent, marksShown, marksBaseline;
	thread_act_t mainThread;
	pid_t child;
} display;

typedef struct {
	const char *name;
	double start, threadCPU, processCPU, childCPU;
	unsigned long long ttyBytes, ttyEvents;
	double ttySeconds, blocked;
	unsigned long draws, presents, skipped, glideFrames, glideMissed;
	unsigned long watchdogFrames;	/* glideWatchdogFrames at the start */
	unsigned long stalls8, stalls16, stalls33;
	double maxStall;
	Samples drawMs, presentMs, latencyMs;
} Phase;

static Phase phase;
static pthread_mutex_t phaseLock = PTHREAD_MUTEX_INITIALIZER;

static double
threadCPUSeconds(thread_act_t thread)
{
	thread_basic_info_data_t info;
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;

	if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&info,
	    &count) != KERN_SUCCESS)
		return 0;
	return info.user_time.seconds + info.user_time.microseconds / 1e6 +
	    info.system_time.seconds + info.system_time.microseconds / 1e6;
}

static double
processCPUSeconds(pid_t process)
{
	struct rusage_info_v2 info;
	static mach_timebase_info_data_t timebase;

	if (process <= 0 || proc_pid_rusage(process, RUSAGE_INFO_V2,
	    (rusage_info_t *)&info))
		return 0;
	if (!timebase.denom)
		mach_timebase_info(&timebase);
	return (double)(info.ri_user_time + info.ri_system_time) *
	    timebase.numer / timebase.denom / 1e9;
}

static void
phaseBegin(const char *name)
{
	pthread_mutex_lock(&phaseLock);
	free(phase.drawMs.items);
	free(phase.presentMs.items);
	free(phase.latencyMs.items);
	memset(&phase, 0, sizeof(phase));
	phase.name = name;
	phase.start = CACurrentMediaTime();
	phase.threadCPU = threadCPUSeconds(display.mainThread);
	phase.processCPU = processCPUSeconds(getpid());
	phase.childCPU = processCPUSeconds(display.child);
	phase.ttyBytes = ttyStats.bytes;
	phase.ttyEvents = ttyStats.events;
	phase.ttySeconds = ttyStats.seconds;
	phase.watchdogFrames = glideWatchdogFrames;
	ttyStats.longest = 0;
	pthread_mutex_unlock(&phaseLock);
}

static void
phaseEnd(double origin)
{
	double now = CACurrentMediaTime(), wall;

	pthread_mutex_lock(&phaseLock);
	wall = MAX(1e-6, now - phase.start);
	if (!phase.name) {
		pthread_mutex_unlock(&phaseLock);
		return;
	}
	printf("phase %s %.1f-%.1fs\n", phase.name, phase.start - origin,
	    now - origin);
	printf("  frames: drawn %lu (%.0f/s) shown %lu (%.0f/s) skipped %lu; "
	    "draw ms p50 %.2f p95 %.2f p99 %.2f max %.2f; blocked on drawable "
	    "%.0f ms\n", phase.draws, phase.draws / wall, phase.presents,
	    phase.presents / wall, phase.skipped,
	    samplesPercentile(&phase.drawMs, 50),
	    samplesPercentile(&phase.drawMs, 95),
	    samplesPercentile(&phase.drawMs, 99),
	    samplesPercentile(&phase.drawMs, 100), phase.blocked * 1000);
	printf("  shown-frame interval ms p50 %.2f p95 %.2f p99 %.2f max %.2f; "
	    "glide frames %lu, late %lu (%.1f%%)\n",
	    samplesPercentile(&phase.presentMs, 50),
	    samplesPercentile(&phase.presentMs, 95),
	    samplesPercentile(&phase.presentMs, 99),
	    samplesPercentile(&phase.presentMs, 100), phase.glideFrames,
	    phase.glideMissed, phase.glideFrames ?
	    100.0 * phase.glideMissed / phase.glideFrames : 0.0);
	printf("  glide watchdog: %lu frames drawn by the timer%s\n",
	    glideWatchdogFrames - phase.watchdogFrames,
	    glideTimerPaced ? ", glides timer-paced" : "");
	printf("  main thread: busy %.0f%%, stalls >8ms %lu >16ms %lu >33ms %lu "
	    "max %.1f ms; st CPU %.0f%%, child CPU %.0f%%\n",
	    100 * (threadCPUSeconds(display.mainThread) - phase.threadCPU) / wall,
	    phase.stalls8, phase.stalls16, phase.stalls33, phase.maxStall,
	    100 * (processCPUSeconds(getpid()) - phase.processCPU) / wall,
	    100 * (processCPUSeconds(display.child) - phase.childCPU) / wall);
	printf("  pty: %.1f KiB/s in %.0f events/s, handling %.0f ms/s, "
	    "longest %.1f ms\n",
	    (ttyStats.bytes - phase.ttyBytes) / 1024.0 / wall,
	    (ttyStats.events - phase.ttyEvents) / wall,
	    (ttyStats.seconds - phase.ttySeconds) * 1000 / wall,
	    ttyStats.longest * 1000);
	if (phase.latencyMs.count)
		printf("  key-to-screen ms p50 %.1f p95 %.1f max %.1f (%zu keys)\n",
		    samplesPercentile(&phase.latencyMs, 50),
		    samplesPercentile(&phase.latencyMs, 95),
		    samplesPercentile(&phase.latencyMs, 100),
		    phase.latencyMs.count);
	fflush(stdout);
	phase.name = NULL;
	pthread_mutex_unlock(&phaseLock);
}

static void
slotRelease(DisplaySlot *slot)
{
	slot->state = SLOT_FREE;
	dispatch_semaphore_signal(display.free);
}

static void *
displayAcquire(void)
{
	double begin = CACurrentMediaTime();
	DisplaySlot *slot = NULL;

	if (dispatch_semaphore_wait(display.free, dispatch_time(
	    DISPATCH_TIME_NOW, NSEC_PER_SEC)))
		return NULL;	/* nextDrawable times out after a second */
	pthread_mutex_lock(&display.lock);
	for (int i = 0; i < DISPLAY_SLOTS && !slot; i++)
		if (display.slots[i].state == SLOT_FREE)
			slot = &display.slots[i];
	slot->state = SLOT_DRAWING;
	slot->sequence = ++display.sequence;
	display.drawing = slot;
	pthread_mutex_unlock(&display.lock);
	pthread_mutex_lock(&phaseLock);
	phase.blocked += CACurrentMediaTime() - begin;
	pthread_mutex_unlock(&phaseLock);
	return (__bridge void *)slot->texture;
}

static void
displayDone(void *texture)
{
	pthread_mutex_lock(&display.lock);
	for (int i = 0; i < DISPLAY_SLOTS; i++)
		if ((__bridge void *)display.slots[i].texture == texture)
			display.slots[i].state = SLOT_DONE;
	pthread_mutex_unlock(&display.lock);
}

/* A vblank: show the oldest finished frame; the one it replaces becomes
 * free.  An unpaused view gets its next draw. */
static void
vblankTick(void)
{
	double now = CACurrentMediaTime();
	DisplaySlot *next = NULL, *old;

	pthread_mutex_lock(&display.lock);
	for (int i = 0; i < DISPLAY_SLOTS; i++)
		if (display.slots[i].state == SLOT_DONE &&
		    (!next || display.slots[i].sequence < next->sequence))
			next = &display.slots[i];
	if (next) {
		old = display.shown;
		next->state = SLOT_SHOWN;
		display.shown = next;
		if (old && old->held)
			old->releaseWhenUnheld = 1;
		else if (old)
			slotRelease(old);
		pthread_mutex_lock(&phaseLock);
		phase.presents++;
		if (display.lastPresent && now - display.lastPresent < 0.25)
			samplesAdd(&phase.presentMs, (now - display.lastPresent) * 1000);
		if (next->moving) {
			phase.glideFrames++;
			if (display.lastMoving && display.lastPresent &&
			    now - display.lastPresent > display.period * 1.5)
				phase.glideMissed++;
		}
		while (display.marksShown < next->marks &&
		    display.marksShown < display.marksSent) {
			samplesAdd(&phase.latencyMs,
			    (now - display.markSent[display.marksShown]) * 1000);
			display.marksShown++;
		}
		pthread_mutex_unlock(&phaseLock);
		display.lastPresent = now;
		display.lastMoving = next->moving;
	}
	pthread_mutex_unlock(&display.lock);
	if (!display.view.isPaused && !display.vblankDrawQueued) {
		display.vblankDrawQueued = 1;
		dispatch_async(dispatch_get_main_queue(), ^{
			display.vblankDrawQueued = 0;
			if (!display.view.isPaused)
				[display.view headlessDraw];
		});
	}
}

static int
countMarks(void)
{
	int count = 0;
	for (int y = 0; y < trow(); y++) {
		Line line = tlineviewline(y);
		for (int x = 0; line && x < tcol(); x++)
			count += line[x].u == 0xa4;
	}
	return count;
}

@implementation HeadlessView {
	BOOL headlessPaused, headlessOnDemand;
}
- (BOOL)isPaused { return headlessPaused; }
- (void)setPaused:(BOOL)paused { headlessPaused = paused; }
- (BOOL)enableSetNeedsDisplay { return headlessOnDemand; }
- (void)setEnableSetNeedsDisplay:(BOOL)value { headlessOnDemand = value; }
- (void)setNeedsDisplay:(BOOL)value
{
	if (value && headlessOnDemand)
		display.needsDisplay = 1;
}

/* The glide watchdog's frames go through the measured path too. */
- (void)glideDraw
{
	[self headlessDraw];
}

- (void)headlessDraw
{
	double begin = CACurrentMediaTime(), blocked;
	DisplaySlot *slot;
	int marks;

	display.needsDisplay = 0;
	pthread_mutex_lock(&phaseLock);
	blocked = phase.blocked;
	pthread_mutex_unlock(&phaseLock);
	display.drawing = NULL;
	[self drawInMTKView:self];
	/* The Nth "mark" key has reached the screen once the grid holds N
	 * more U+00A4 than when the first was typed. */
	marks = display.marksSent ? countMarks() - display.marksBaseline : 0;
	pthread_mutex_lock(&display.lock);
	slot = display.drawing;
	if (slot) {
		slot->moving = smoothscroll_pending();
		slot->marks = MAX(0, marks);
	}
	pthread_mutex_unlock(&display.lock);
	pthread_mutex_lock(&phaseLock);
	if (slot) {
		phase.draws++;
		samplesAdd(&phase.drawMs, (CACurrentMediaTime() - begin -
		    (phase.blocked - blocked)) * 1000);
	} else {
		phase.skipped++;
	}
	pthread_mutex_unlock(&phaseLock);
}
@end

typedef struct {
	double at;
	const char *name;
} PhaseMark;
static PhaseMark phaseMarks[64];
static int phaseMarkCount;

/* Snapshot the frame on screen without holding up the main thread. */
static void
snapshotShown(const char *path, int wait)
{
	DisplaySlot *slot;
	char *copy = strdup(path);
	dispatch_group_t group = dispatch_group_create();

	pthread_mutex_lock(&display.lock);
	slot = display.shown;
	if (slot)
		slot->held = 1;
	pthread_mutex_unlock(&display.lock);
	if (!slot) {
		free(copy);
		return;
	}
	dispatch_group_async(group, dispatch_get_global_queue(
	    QOS_CLASS_UTILITY, 0), ^{
		saveTexture(slot->texture, copy);
		pthread_mutex_lock(&display.lock);
		slot->held = 0;
		if (slot->releaseWhenUnheld) {
			slot->releaseWhenUnheld = 0;
			slotRelease(slot);
		}
		pthread_mutex_unlock(&display.lock);
		free(copy);
	});
	if (wait)
		dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
}

static void
stopChild(void)
{
	pid_t group = display.child;
	int status;

	if (ttySource)
		dispatch_source_cancel(ttySource);
	if (group <= 1 || group == getpgrp())
		return;
	kill(-group, SIGHUP);
	for (int i = 0; i < 30; i++) {
		if (waitpid(-1, &status, WNOHANG) > 0 && kill(-group, 0) < 0)
			return;
		if (i == 10)
			kill(-group, SIGTERM);
		usleep(100000);
	}
	kill(-group, SIGKILL);
	waitpid(-1, &status, WNOHANG);
}

static int
appMain(HeadlessView *view, double seconds, double fps, double every,
		const char *outdir)
{
	double origin = CACurrentMediaTime();
	__block int finished = 0;
	CGSize size = view.drawableSize;
	/* No timer coalescing or App Nap while measuring. */
	id activity = [NSProcessInfo.processInfo beginActivityWithOptions:
	    NSActivityUserInitiated | NSActivityLatencyCritical
	    reason:@"st headless measurement"];

	view.preferredFramesPerSecond = (NSInteger)lrint(fps);
	display.view = view;
	display.child = ttypid();	/* a session leader: its group is its own */
	display.period = 1.0 / fps;
	display.mainThread = mach_thread_self();
	pthread_mutex_init(&display.lock, NULL);
	display.free = dispatch_semaphore_create(DISPLAY_SLOTS);
	for (int i = 0; i < DISPLAY_SLOTS; i++) {
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:(NSUInteger)size.width height:(NSUInteger)size.height
		    mipmapped:NO];
		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeShared;
		display.slots[i].texture = [view.device
		    newTextureWithDescriptor:descriptor];
	}
	mac_renderer_set_offscreen(displayAcquire, displayDone);

	display.vblankQueue = dispatch_queue_create("st.headless.vblank",
	    dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
	    QOS_CLASS_USER_INTERACTIVE, 0));
	display.vblank = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0,
	    0, display.vblankQueue);
	dispatch_source_set_timer(display.vblank, DISPATCH_TIME_NOW,
	    (uint64_t)(display.period * NSEC_PER_SEC), 0);
	dispatch_source_set_event_handler(display.vblank, ^{ vblankTick(); });
	dispatch_resume(display.vblank);

	/* Main-thread responsiveness: how late a 2 ms heartbeat runs. */
	display.lastBeat = CACurrentMediaTime();
	display.heartbeat = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0,
	    0, dispatch_get_main_queue());
	dispatch_source_set_timer(display.heartbeat, DISPATCH_TIME_NOW,
	    2 * NSEC_PER_MSEC, 0);
	dispatch_source_set_event_handler(display.heartbeat, ^{
		double now = CACurrentMediaTime();
		double stall = now - display.lastBeat - 0.002;
		display.lastBeat = now;
		pthread_mutex_lock(&phaseLock);
		phase.stalls8 += stall > 0.008;
		phase.stalls16 += stall > 0.016;
		phase.stalls33 += stall > 0.033;
		phase.maxStall = MAX(phase.maxStall, stall * 1000);
		pthread_mutex_unlock(&phaseLock);
	});
	dispatch_resume(display.heartbeat);

	/* AppKit displays views marked for display when the run loop is
	 * about to wait. */
	CFRunLoopObserverRef observer = CFRunLoopObserverCreateWithHandler(NULL,
	    kCFRunLoopBeforeWaiting | kCFRunLoopExit, true, 2000000,
	    ^(CFRunLoopObserverRef o, CFRunLoopActivity stage) {
		(void)o; (void)stage;
		if (display.needsDisplay && view.enableSetNeedsDisplay)
			[view headlessDraw];
	});
	CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);

	setupTTYSource();
	macos_request_redraw();

	for (int i = 0; i < keyCount; i++) {
		HeadlessKey *key = &keys[i];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
		    (int64_t)(key->at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
			int mark = key->length == 2 && !memcmp(key->bytes, "\302\244", 2);
			if (mark) {
				pthread_mutex_lock(&display.lock);
				if (!display.marksSent)
					display.marksBaseline = countMarks();
				if (display.marksSent < (int)LEN(display.markSent))
					display.markSent[display.marksSent++] =
					    CACurrentMediaTime();
				pthread_mutex_unlock(&display.lock);
			}
			ttywrite(key->bytes, key->length, 1);
		});
	}
	if (!phaseMarkCount) {
		phaseMarks[0] = (PhaseMark){0, "all"};
		phaseMarkCount = 1;
	}
	for (int i = 0; i < phaseMarkCount; i++) {
		PhaseMark *mark = &phaseMarks[i];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
		    (int64_t)(mark->at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
			phaseEnd(origin);
			phaseBegin(mark->name);
		});
	}
	if (every > 0) {
		for (int shot = 0; (shot + 1) * every < seconds; shot++) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
			    (int64_t)((shot + 1) * every * NSEC_PER_SEC)),
			    dispatch_get_main_queue(), ^{
				char path[PATH_MAX];
				snprintf(path, sizeof(path), "%s/shot-%03d.png", outdir, shot);
				snapshotShown(path, 0);
			});
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		finished = 1;
	});

	while (!finished)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);

	char status[1024], path[PATH_MAX];
	phaseEnd(origin);
	dispatch_source_cancel(display.heartbeat);
	snprintf(path, sizeof(path), "%s/shot-final.png", outdir);
	snapshotShown(path, 1);
	mac_video_describe(status, sizeof(status));
	printf("%6.2fs %s images=%zu placements=%zu image_mb=%.1f %s\n",
	    CACurrentMediaTime() - origin, path, graphics_image_count(),
	    graphics_placement_count(), graphics_image_bytes() / 1048576.0,
	    status);
	fflush(stdout);
	stopChild();
	dispatch_source_cancel(display.vblank);
	graphics_reset();
	[NSProcessInfo.processInfo endActivity:activity];
	return 0;
}

int
main(int argc, char **argv)
{
	int columns = 100, rows = 30, audible = 0, app = 0, opt;
	double seconds = 20, every = 2, fps = 60, pointsWide = 0, pointsHigh = 0;
	const char *outdir = "/tmp/st-headless";

	while ((opt = getopt(argc, argv, "c:r:t:s:o:f:ak:K:Ag:p:")) != -1) {
		switch (opt) {
		case 'A': app = 1; break;
		case 'g':
			if (sscanf(optarg, "%lfx%lf", &pointsWide, &pointsHigh) != 2) {
				fprintf(stderr, "bad size %s\n", optarg);
				return 2;
			}
			break;
		case 'p':
			if (phaseMarkCount == (int)LEN(phaseMarks) ||
			    !strchr(optarg, ':')) {
				fprintf(stderr, "bad phase %s\n", optarg);
				return 2;
			}
			phaseMarks[phaseMarkCount++] = (PhaseMark){atof(optarg),
			    strchr(optarg, ':') + 1};
			break;
		case 'K':
			if (!parseRepeat(optarg)) {
				fprintf(stderr, "bad repeat %s\n", optarg);
				return 2;
			}
			break;
		case 'c': columns = atoi(optarg); break;
		case 'r': rows = atoi(optarg); break;
		case 't': seconds = atof(optarg); break;
		case 's': every = atof(optarg); break;
		case 'o': outdir = optarg; break;
		case 'f': fps = atof(optarg); break;
		case 'a': audible = 1; break;
		case 'k':
			if (!parseKey(optarg)) {
				fprintf(stderr, "bad key %s\n", optarg);
				return 2;
			}
			break;
		default:
			fprintf(stderr, "usage: %s [-A] [-c cols] [-r rows] "
			    "[-g WIDTHxHEIGHT points] [-t seconds] [-s snapshot-every] "
			    "[-f fps] [-o dir] [-a] [-k secs:key]... "
			    "[-K start:end:hz:key]... [-p secs:phase]... -- command...\n",
			    argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "%s: no command\n", argv[0]);
		return 2;
	}
	if (!audible)
		setenv("ST_VIDEO_VOLUME", "0", 1);
	mkdir(outdir, 0755);
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		MTKView *view = app ?
		    [[HeadlessView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)
		    device:device] :
		    [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)
		    device:device];
		view.paused = YES;
		view.autoResizeDrawable = NO;
		nativeView = (STMetalView *)view;
		if (!mac_renderer_init((__bridge void *)view, macosDefaultFont,
		    parseFontSize(macosDefaultFont)))
			return 1;
		mac_renderer_set_scale(2.0);
		usedfont = (char *)macosDefaultFont;
		fontWidthSpacing = macosDefaultFontWidthSpacing;
		updateCellMetrics();
		xloadcols();
		if (pointsWide > 0 && pointsHigh > 0) {
			columns = MAX(1, (int)((pointsWide - 2 * borderpx) / win.cw));
			rows = MAX(1, (int)((pointsHigh - 2 * borderpx) / win.ch));
		}
		tnew(columns, rows);
		graphics_set_image_free_callback(freeGraphicsImage, NULL);
		mac_video_init((__bridge void *)device, NULL,
		    app ? videoRedraw : headlessRedraw);
		if (app)
			smoothscroll_configure(smoothscroll, smoothscroll_ms,
			    smoothscroll_regions);

		double width = 2 * borderpx + columns * win.cw;
		double height = 2 * borderpx + rows * win.ch;
		view.drawableSize = CGSizeMake(width * 2, height * 2);
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:(NSUInteger)(width * 2) height:(NSUInteger)(height * 2)
		    mipmapped:NO];
		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeShared;
		headlessTarget = [device newTextureWithDescriptor:descriptor];

		opt_cmd = argv + optind;
		ttyfd = ttynew(NULL, "/bin/sh", NULL, opt_cmd);
		if (macos_pty_start(ttyfd) < 0)
			return 1;
		if (app) {
			printf("pid %d, %dx%d cells, %.0fx%.0f px\n", (int)getpid(),
			    columns, rows, width * 2, height * 2);
			fflush(stdout);
			nativeResize(width, height);
			return appMain((HeadlessView *)view, seconds, fps, every,
			    outdir);
		}
		nativeView = nil;	/* no window to invalidate */
		nativeResize(width, height);

		double start = CACurrentMediaTime(), nextShot = start + every;
		double nextFrame = start;
		int shot = 0;
		char status[1024], path[PATH_MAX], laststate[32] = "";
		while (CACurrentMediaTime() - start < seconds) {
			struct pollfd readable = {ttyfd, POLLIN, 0};
			int status_code;
			if (poll(&readable, 1, 2) > 0) {
				/* As the backend's tty source does: drain for ~4 ms. */
				if (readable.revents & POLLIN) {
					double deadline = CACurrentMediaTime() + 0.004;
					while (ttyread() > 0 && CACurrentMediaTime() < deadline)
						;
				} else if (readable.revents & (POLLHUP | POLLERR)) {
					break;
				}
			}
			if (waitpid(-1, &status_code, WNOHANG) > 0)
				break;
			/* Main-queue work (decoder callbacks) without blocking. */
			[NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode
			    beforeDate:NSDate.distantPast];
			double now = CACurrentMediaTime();
			for (int i = 0; i < keyCount; i++)
				if (!keys[i].sent && now - start >= keys[i].at) {
					keys[i].sent = 1;
					ttywrite(keys[i].bytes, keys[i].length, 1);
					printf("%6.3fs key %s\n", now - start,
					    keys[i].bytes[0] == '\033' ? "<esc seq>" :
					    keys[i].bytes);
					fflush(stdout);
				}
			mac_video_tick(now + 1.0 / fps);
			/* Every change of the first video's state, timestamped. */
			mac_video_describe(status, sizeof(status));
			{
				char state[32];
				snprintf(state, sizeof(state), "%.*s",
				    (int)strcspn(status, " ;"), status);
				if (strcmp(state, laststate)) {
					printf("%6.3fs state %s (%s)\n", now - start,
					    *state ? state : "none", status);
					fflush(stdout);
					snprintf(laststate, sizeof(laststate), "%s", state);
				}
			}
			if (now >= nextFrame) {
				renderTerminal();
				nextFrame = now + 1.0 / fps;
			}
			if (now >= nextShot) {
				snprintf(path, sizeof(path), "%s/shot-%03d.png", outdir, shot++);
				saveSnapshot(path);
				mac_video_describe(status, sizeof(status));
				printf("%6.2fs %s frames=%d images=%zu placements=%zu "
				    "image_mb=%.1f %s\n", now - start, path, headlessFrames,
				    graphics_image_count(), graphics_placement_count(),
				    graphics_image_bytes() / 1048576.0, status);
				fflush(stdout);
				nextShot += every;
			}
		}
		renderTerminal();
		snprintf(path, sizeof(path), "%s/shot-final.png", outdir);
		saveSnapshot(path);
		mac_video_describe(status, sizeof(status));
		printf("%6.2fs %s frames=%d %s\n", CACurrentMediaTime() - start, path,
		    headlessFrames, status);
		graphics_reset();
	}
	return 0;
}

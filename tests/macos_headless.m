/* A windowless st for end-to-end checks on macOS: runs a command in a PTY
 * through st's real terminal core, Kitty graphics and video players, renders
 * frames with the real Metal renderer offscreen and saves PNG snapshots.
 *
 *   tests/macos_headless -c 100 -r 30 -t 20 -s 2 -o /tmp/shots -- \
 *       ssh -t whale st-play clip.mp4
 *
 * Sound plays unless ST_VIDEO_VOLUME=0 (the harness sets it unless -a).
 * Each snapshot prints the video players' states.  Not part of `make test`. */
#define main st_backend_main
#include "../macos/backend.m"
#undef main
#include "../macos/renderer.m"
#import <ImageIO/ImageIO.h>
#include <poll.h>
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
static HeadlessKey keys[64];
static int keyCount;

static int
parseKey(const char *spec)
{
	static const struct { const char *name, *bytes; } named[] = {
		{"space", " "}, {"left", "\033[D"}, {"right", "\033[C"},
		{"up", "\033[A"}, {"down", "\033[B"}, {"esc", "\033"},
		{"enter", "\r"},
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
saveSnapshot(const char *path)
{
	size_t width = headlessTarget.width, height = headlessTarget.height;
	unsigned char *pixels = malloc(width * height * 4);
	[headlessTarget getBytes:pixels bytesPerRow:width * 4
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

int
main(int argc, char **argv)
{
	int columns = 100, rows = 30, audible = 0, opt;
	double seconds = 20, every = 2, fps = 60;
	const char *outdir = "/tmp/st-headless";

	while ((opt = getopt(argc, argv, "c:r:t:s:o:f:ak:")) != -1) {
		switch (opt) {
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
			fprintf(stderr, "usage: %s [-c cols] [-r rows] [-t seconds] "
			    "[-s snapshot-every] [-f fps] [-o dir] [-a] [-k secs:key]... "
			    "-- command...\n",
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
		MTKView *view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)
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
		tnew(columns, rows);
		graphics_set_image_free_callback(freeGraphicsImage, NULL);
		mac_video_init((__bridge void *)device, NULL, headlessRedraw);

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

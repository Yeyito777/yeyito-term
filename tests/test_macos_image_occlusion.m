/* Exercise the real Cocoa cell emitter and Metal frame encoder offscreen.
 * No test window is opened and no screen-recording permission is required. */
#define main st_backend_main
#include "../macos/backend.m"
#undef main
#include "../macos/renderer.m"
#include "test.h"

static id<MTLTexture> target;
static unsigned char pixels[128 * 128 * 4];
static GraphicsPlacementView imagePlacement;
static GraphicsPlacementView upperPlacement;
static const unsigned char redPixel[] = {255, 0, 0, 255};
static const unsigned char greenPixel[] = {0, 255, 0, 255};

static void
resetScene(void)
{
	image_occlusion_clear(&imageOcclusion);
	upperPlacement = (GraphicsPlacementView){0};
	for (int y = 0; y < trow(); y++)
		for (int x = 0; x < tcol(); x++)
			tlineviewline(y)[x] = (Glyph){.u = ' ',
			    .fg = defaultfg, .bg = defaultbg};
	imagePlacement = (GraphicsPlacementView){.serial = 1, .image_id = 1,
	    .placement_id = 1, .rgba = redPixel, .image_width = 1,
	    .image_height = 1, .source_width = 1, .source_height = 1,
	    .anchor = tlineviewline(1), .column = 1, .row = 1,
	    .columns = 4, .rows = 2, .z = 0};
}

static void
renderScene(int cursor)
{
	for (int i = 0; i < MAC_LAYER_COUNT; i++)
		listreset(&r.layers[i]);
	for (int i = 0; i < 3; i++)
		r.imageLayers[i].count = 0;
	image_occlusion_begin(&imageOcclusion);
	for (int y = 0; y < trow(); y++)
		xdrawline(tlineviewline(y), 0, y, tcol());
	mac_renderer_set_image_clip(borderpx, borderpx, win.tw, win.th);
	drawGraphicsPlacement(&imagePlacement,
	    (void *)(intptr_t)GRAPHICS_STAGE_ABOVE_TEXT);
	if (upperPlacement.rgba)
		drawGraphicsPlacement(&upperPlacement,
		    (void *)(intptr_t)GRAPHICS_STAGE_ABOVE_TEXT);
	image_occlusion_end(&imageOcclusion);
	if (cursor) {
		Glyph g = {.u = ' ', .fg = defaultfg, .bg = TRUECOLOR(0, 255, 0)};
		drawCell(g, 1, 1, MAC_CELL_OVERLAY, 0);
	}
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	id<MTLCommandBuffer> command = [r.queue commandBuffer];
	id<MTLRenderCommandEncoder> encoder =
	    [command renderCommandEncoderWithDescriptor:pass];
	encodeFrame(encoder);
	[encoder endEncoding];
	[command commit];
	[command waitUntilCompleted];
	if (command.status == MTLCommandBufferStatusError) {
		fprintf(stderr, "Metal test failed: %s\n", command.error.description.UTF8String);
		exit(1);
	}
	[target getBytes:pixels bytesPerRow:128 * 4
	    fromRegion:MTLRegionMake2D(0, 0, 128, 128) mipmapLevel:0];
}

static int
cellIsColor(int x, int y, int red, int green, int blue)
{
	int px = (int)cellX(x) + win.cw / 2;
	int py = (int)cellY(y) + win.ch / 2;
	unsigned char *p = &pixels[(py * 128 + px) * 4];
	return p[0] == blue && p[1] == green && p[2] == red;
}

TEST(metal_menu_covers_image_and_restoration_reveals_it)
{
	resetScene();
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 255, 0, 0));
	Line line = tlineviewline(1);
	line[1].bg = TRUECOLOR(0, 0, 255);
	line[2].u = 0x2588;
	line[2].fg = TRUECOLOR(255, 255, 255);
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 0, 0, 255));
	ASSERT(cellIsColor(2, 1, 255, 255, 255));
	ASSERT(cellIsColor(3, 1, 255, 0, 0));
	line[1].bg = defaultbg;
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 255, 0, 0));
	ASSERT(cellIsColor(2, 1, 255, 255, 255));
	line[2].u = ' ';
	line[2].fg = defaultfg;
	renderScene(0);
	ASSERT(cellIsColor(2, 1, 255, 0, 0));
}

TEST(metal_default_background_blank_is_opaque_above_image)
{
	resetScene();
	tlineviewline(1)[1].u = 'x';
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 255, 0, 0));
	tlineviewline(1)[1].u = ' ';
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 0, 0, 0));
	ASSERT(cellIsColor(2, 1, 255, 0, 0));
}

TEST(metal_cursor_above_occlusion_and_selection_keeps_baseline)
{
	resetScene();
	renderScene(0);
	tlineviewline(1)[1].bg = TRUECOLOR(0, 0, 255);
	renderScene(1);
	ASSERT(cellIsColor(1, 1, 0, 255, 0));
	imagePlacement.selected = 1;
	renderScene(0);
	ASSERT_EQ(0, r.layers[MAC_LAYER_IMAGE_BACKGROUND].count);
	ASSERT_EQ(8, imageOcclusion.cells);
	imagePlacement.selected = 0;
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 0, 0, 255));
}

TEST(metal_stacked_images_keep_masks_and_tints_below_higher_image)
{
	resetScene();
	renderScene(0);
	/* The modal's blue border/blank cells mask the existing red preview. */
	tlineviewline(1)[1].bg = TRUECOLOR(0, 0, 255);
	tlineviewline(1)[2].bg = TRUECOLOR(0, 0, 255);
	upperPlacement = imagePlacement;
	upperPlacement.serial = upperPlacement.image_id = 2;
	upperPlacement.rgba = greenPixel;
	upperPlacement.column = 2;
	upperPlacement.columns = 2;
	upperPlacement.z = 2;
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 0, 0, 255)); /* border */
	ASSERT(cellIsColor(2, 1, 0, 255, 0)); /* not lower image's blue mask */
	ASSERT(cellIsColor(4, 1, 255, 0, 0)); /* untouched preview */
	/* Upper images still get their own text masks. */
	tlineviewline(1)[2].bg = TRUECOLOR(255, 255, 255);
	renderScene(0);
	ASSERT(cellIsColor(2, 1, 255, 255, 255));
	tlineviewline(1)[2].bg = TRUECOLOR(0, 0, 255);
	imagePlacement.selected = 1;
	renderScene(0);
	ASSERT(cellIsColor(2, 1, 0, 255, 0)); /* lower selection cannot tint modal */
	imagePlacement.selected = 0;
	upperPlacement.rgba = NULL;
	tlineviewline(1)[1].bg = tlineviewline(1)[2].bg = defaultbg;
	renderScene(0);
	ASSERT(cellIsColor(1, 1, 255, 0, 0));
	ASSERT(cellIsColor(2, 1, 255, 0, 0)); /* closing restores original image */
}

int
main(void)
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device) {
			puts("SKIP: Metal image compositing tests require a Metal device");
			return 0;
		}
		MTKView *view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128)
		    device:device];
		view.paused = YES;
		view.autoResizeDrawable = NO;
		view.drawableSize = CGSizeMake(128, 128);
		if (!mac_renderer_init((__bridge void *)view, "Menlo", 12))
			return 1;
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:128 height:128 mipmapped:NO];
		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeShared;
		target = [device newTextureWithDescriptor:descriptor];
		if (!target)
			return 1;
		xloadcols();
		palette[defaultbg] = (MacColor){0, 0, 0, 1};
		palette[defaultfg] = (MacColor){1, 1, 1, 1};
		tnew(8, 4);
		win.cw = win.ch = 16;
		win.tw = 8 * win.cw;
		win.th = 4 * win.ch;
		RUN_TEST(metal_menu_covers_image_and_restoration_reveals_it);
		RUN_TEST(metal_default_background_blank_is_opaque_above_image);
		RUN_TEST(metal_cursor_above_occlusion_and_selection_keeps_baseline);
		RUN_TEST(metal_stacked_images_keep_masks_and_tints_below_higher_image);
		image_occlusion_clear(&imageOcclusion);
		mac_renderer_destroy();
		return test_summary();
	}
}

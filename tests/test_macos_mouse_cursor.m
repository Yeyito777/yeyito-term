/* OSC 777 mouseshape on the native macOS backend: the app-chosen shape is
 * what the view's cursor rect and cursor updates use, and st never takes
 * the cursor while it has no key window under the pointer.
 * No test window is opened. */
#define main st_backend_main
#include "../macos/backend.m"
#undef main
#include "../macos/renderer.m"
#include "test.h"

@interface STCursorRectRecorder : STMetalView
@property(nonatomic, strong) NSCursor *recorded;
@property(nonatomic) NSRect recordedRect;
@end

@implementation STCursorRectRecorder
- (void)addCursorRect:(NSRect)rect cursor:(NSCursor *)cursor
{
	self.recordedRect = rect;
	self.recorded = cursor;
}
@end

static id<MTLDevice> device;

/* System cursors need not be singletons; compare what they look like. */
static BOOL
sameCursor(NSCursor *a, NSCursor *b)
{
	if (a == b) return YES;
	if (!a || !b || !NSEqualPoints(a.hotSpot, b.hotSpot)) return NO;
	return [a.image.TIFFRepresentation isEqualToData:b.image.TIFFRepresentation];
}

TEST(osc_shapes_map_to_cocoa_cursors)
{
	xsetmousecursor(0);
	ASSERT(sameCursor(nativeMouseCursor(), NSCursor.IBeamCursor));
	xsetmousecursor(1);
	ASSERT(sameCursor(nativeMouseCursor(), NSCursor.arrowCursor));
	xsetmousecursor(2);
	ASSERT(sameCursor(nativeMouseCursor(), NSCursor.pointingHandCursor));
	xsetmousecursor(0);
	ASSERT(sameCursor(nativeMouseCursor(), NSCursor.IBeamCursor));
}

TEST(no_key_window_never_claims_the_pointer)
{
	ASSERT(nativeWindow == nil);
	ASSERT(!nativePointerInView());
	/* Safe before the window exists, e.g. a shape sent during startup. */
	xsetmousecursor(2);
	ASSERT_EQ(2, cursorKind);
	xsetmousecursor(0);
}

TEST(cursor_rect_covers_view_with_current_shape)
{
	STCursorRectRecorder *view = [[STCursorRectRecorder alloc]
	    initWithFrame:NSMakeRect(0, 0, 200, 100) device:device];
	ASSERT(view != nil);
	xsetmousecursor(2);
	[view resetCursorRects];
	ASSERT(sameCursor(view.recorded, NSCursor.pointingHandCursor));
	ASSERT(NSEqualRects(view.recordedRect, view.bounds));
	xsetmousecursor(1);
	[view resetCursorRects];
	ASSERT(sameCursor(view.recorded, NSCursor.arrowCursor));
	xsetmousecursor(0);
	[view resetCursorRects];
	ASSERT(sameCursor(view.recorded, NSCursor.IBeamCursor));
}

TEST(view_tracks_motion_and_cursor_updates)
{
	STMetalView *view = [[STMetalView alloc]
	    initWithFrame:NSMakeRect(0, 0, 200, 100) device:device];
	ASSERT(view != nil);
	NSTrackingAreaOptions wanted = NSTrackingMouseMoved |
	    NSTrackingCursorUpdate | NSTrackingInVisibleRect;
	BOOL tracked = NO;
	for (NSTrackingArea *area in view.trackingAreas)
		if (area.owner == view && (area.options & wanted) == wanted)
			tracked = YES;
	ASSERT(tracked);
	ASSERT([view respondsToSelector:@selector(cursorUpdate:)]);
	ASSERT([view respondsToSelector:@selector(mouseMoved:)]);
}

int
main(void)
{
	@autoreleasepool {
		printf("macOS mouse cursor tests\n");
		RUN_TEST(osc_shapes_map_to_cocoa_cursors);
		RUN_TEST(no_key_window_never_claims_the_pointer);
		device = MTLCreateSystemDefaultDevice();
		if (!device) {
			puts("SKIP: view cursor tests require a Metal device");
			return test_summary();
		}
		RUN_TEST(cursor_rect_covers_view_with_current_shape);
		RUN_TEST(view_tracks_motion_and_cursor_updates);
		return test_summary();
	}
}

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

TEST(shift_mouse_reaches_apps_option_forces_selection)
{
	uint saved = win.mode;
	win.mode |= MODE_MOUSEBTN | MODE_MOUSESGR;
	ASSERT(appGetsMouse(0));
	ASSERT(appGetsMouse(ShiftMask));
	ASSERT(appGetsMouse(ShiftMask | ControlMask));
	ASSERT(!appGetsMouse(Mod1Mask));
	ASSERT(!appGetsMouse(ShiftMask | Mod1Mask));
	/* Option+drag is a plain native selection, not a rectangular one. */
	ASSERT_EQ(SEL_REGULAR, selectionType(Button1Mask | Mod1Mask));
	win.mode &= ~MODE_MOUSE;
	ASSERT(!appGetsMouse(0));
	ASSERT(!appGetsMouse(ShiftMask));
	/* Without app mouse reporting, Option+drag stays rectangular. */
	win.mode = saved;
}

static char reports[256];
static size_t reportsLength;
static int reportCount;

static void
captureReport(const char *s, size_t n, int mayEcho)
{
	(void)mayEcho;
	if (reportsLength + n < sizeof(reports)) {
		memcpy(reports + reportsLength, s, n);
		reportsLength += n;
		reports[reportsLength] = '\0';
	}
	reportCount++;
}

static void
resetReports(void)
{
	reportsLength = 0;
	reports[0] = '\0';
	reportCount = 0;
}

TEST(modifier_change_reports_one_motion_with_its_bits)
{
	uint saved = win.mode;
	mouseReportWrite = captureReport;
	pressedButtons = 0;
	win.mode = (win.mode & ~MODE_MOUSE) | MODE_MOUSEMANY | MODE_MOUSESGR;

	/* Shift pressed over cell (4, 2): one SGR motion, button 3 (none) + 32 + Shift 4. */
	resetReports();
	modifierMotion(ShiftMask, 4, 2);
	ASSERT_EQ(1, reportCount);
	ASSERT(strcmp(reports, "\033[<39;5;3M") == 0);
	/* The same modifiers again, or only ones st does not report (Caps Lock, Command): nothing. */
	resetReports();
	modifierMotion(ShiftMask, 4, 2);
	modifierMotion(ShiftMask | LockMask | Mod4Mask, 4, 2);
	ASSERT_EQ(0, reportCount);
	/* Shift let go: one plain motion. Control: its own bit. */
	resetReports();
	modifierMotion(0, 4, 2);
	ASSERT(strcmp(reports, "\033[<35;5;3M") == 0);
	resetReports();
	modifierMotion(ControlMask, 4, 2);
	ASSERT(strcmp(reports, "\033[<51;5;3M") == 0);

	/* Without 1006, the legacy encoding. */
	win.mode &= ~MODE_MOUSESGR;
	resetReports();
	modifierMotion(ShiftMask, 4, 2);
	ASSERT_EQ(1, reportCount);
	ASSERT(reportsLength == 6 && memcmp(reports, "\033[M", 3) == 0);
	ASSERT_EQ(32 + 39, (unsigned char)reports[3]);
	ASSERT_EQ(32 + 5, (unsigned char)reports[4]);
	ASSERT_EQ(32 + 3, (unsigned char)reports[5]);
	win.mode |= MODE_MOUSESGR;

	/* 1002 reports motion only with a button held. */
	win.mode = (win.mode & ~MODE_MOUSE) | MODE_MOUSEMOTION | MODE_MOUSESGR;
	resetReports();
	modifierMotion(0, 4, 2);
	ASSERT_EQ(0, reportCount);
	pressedButtons = 1U << (Button1 - 1);
	modifierMotion(ControlMask, 4, 2);
	ASSERT(strcmp(reports, "\033[<48;5;3M") == 0);
	/* A drag that forces native selection is st's own. */
	resetReports();
	modifierMotion(forcemousemod, 4, 2);
	ASSERT_EQ(0, reportCount);
	pressedButtons = 0;

	/* Mouse reporting off: nothing. */
	win.mode &= ~MODE_MOUSE;
	resetReports();
	modifierMotion(ShiftMask, 6, 2);
	modifierMotion(0, 6, 2);
	ASSERT_EQ(0, reportCount);

	win.mode = saved;
	mouseReportWrite = ttywrite;
}

TEST(flags_change_without_the_pointer_in_a_window_reports_nothing)
{
	uint saved = win.mode;
	STMetalView *view = [[STMetalView alloc]
	    initWithFrame:NSMakeRect(0, 0, 200, 100) device:device];
	mouseReportWrite = captureReport;
	win.mode = (win.mode & ~MODE_MOUSE) | MODE_MOUSEMANY | MODE_MOUSESGR;
	resetReports();
	NSEvent *shift = [NSEvent keyEventWithType:NSEventTypeFlagsChanged
	    location:NSZeroPoint modifierFlags:NSEventModifierFlagShift timestamp:0
	    windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@""
	    isARepeat:NO keyCode:56];
	[view flagsChanged:shift];
	ASSERT_EQ(0, reportCount);
	win.mode = saved;
	mouseReportWrite = ttywrite;
}

int
main(void)
{
	@autoreleasepool {
		printf("macOS mouse cursor tests\n");
		RUN_TEST(osc_shapes_map_to_cocoa_cursors);
		RUN_TEST(no_key_window_never_claims_the_pointer);
		RUN_TEST(shift_mouse_reaches_apps_option_forces_selection);
		RUN_TEST(modifier_change_reports_one_motion_with_its_bits);
		device = MTLCreateSystemDefaultDevice();
		if (!device) {
			puts("SKIP: view cursor tests require a Metal device");
			return test_summary();
		}
		RUN_TEST(cursor_rect_covers_view_with_current_shape);
		RUN_TEST(view_tracks_motion_and_cursor_updates);
		RUN_TEST(flags_change_without_the_pointer_in_a_window_reports_nothing);
		return test_summary();
	}
}

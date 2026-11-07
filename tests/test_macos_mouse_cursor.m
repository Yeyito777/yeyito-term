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

TEST(modifier_changes_report_only_with_mode_7778)
{
	uint saved = win.mode;
	mouseReportWrite = captureReport;
	pressedButtons = 0;
	/* Any-motion SGR mouse tracking on, as Exocortex runs. */
	win.mode = (win.mode & ~(MODE_MOUSE | MODE_MODREPORT)) | MODE_MOUSEMANY | MODE_MOUSESGR;

	/* Mode off: Shift down and up send nothing, and never a mouse motion. */
	resetReports();
	modifierChange(ShiftMask, 1, 4, 2);
	modifierChange(0, 1, 4, 2);
	ASSERT_EQ(0, reportCount);

	/* Mode on: one report per change, with Shift/Option/Control as 1/2/4 and a 1-based cell. */
	win.mode |= MODE_MODREPORT;
	resetReports();
	modifierChange(ShiftMask, 1, 4, 2);
	ASSERT_EQ(1, reportCount);
	ASSERT(strcmp(reports, "\033[?7778;1;5;3~") == 0);
	/* Caps Lock or Command alone changes nothing reported. */
	resetReports();
	modifierChange(ShiftMask | LockMask, 1, 4, 2);
	modifierChange(ShiftMask | LockMask | Mod4Mask, 1, 4, 2);
	ASSERT_EQ(0, reportCount);
	resetReports();
	modifierChange(ShiftMask | ControlMask | Mod1Mask, 1, 7, 0);
	ASSERT(strcmp(reports, "\033[?7778;7;8;1~") == 0);
	resetReports();
	modifierChange(0, 1, 7, 0);
	ASSERT(strcmp(reports, "\033[?7778;0;8;1~") == 0);
	ASSERT(strstr(reports, "\033[<") == NULL);

	/* The pointer outside the view: nothing, and the next change inside still reports. */
	resetReports();
	modifierChange(ShiftMask, 0, 0, 0);
	ASSERT_EQ(0, reportCount);
	modifierChange(0, 1, 4, 2);
	ASSERT_EQ(1, reportCount);
	ASSERT(strcmp(reports, "\033[?7778;0;5;3~") == 0);

	/* An application boundary clears the mode. */
	ASSERT(MODE_APPRESET & MODE_MODREPORT);
	win.mode = saved;
	mouseReportWrite = ttywrite;
}

TEST(flags_change_without_the_pointer_in_a_window_reports_nothing)
{
	uint saved = win.mode;
	STMetalView *view = [[STMetalView alloc]
	    initWithFrame:NSMakeRect(0, 0, 200, 100) device:device];
	mouseReportWrite = captureReport;
	win.mode = (win.mode & ~MODE_MOUSE) | MODE_MOUSEMANY | MODE_MOUSESGR | MODE_MODREPORT;
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
		RUN_TEST(modifier_changes_report_only_with_mode_7778);
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

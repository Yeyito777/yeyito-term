/* Real production input dispatch with private, unposted Unicode events.
 * No window is opened, no clipboard is touched, no user input is sent. */
#define main st_backend_main
#define ttywrite captureWrite
#define tisaltscreen testAltScreen
#include "../macos/backend.m"
#undef main
#undef ttywrite
#undef tisaltscreen
#include "../macos/renderer.m"
#define main symbol_helper_main
#include "../scripts/macos-type-symbol.m"
#undef main
#include "test.h"

static char output[256];
static int altScreen;

void
captureWrite(const char *data, size_t length, int echo)
{
	(void)echo;
	size_t used = strlen(output);
	if (length + used >= sizeof output) abort();
	memcpy(output + used, data, length);
	output[length + used] = '\0';
}

int testAltScreen(void) { return altScreen; }

TEST(unicode_events_preserve_text_and_clear_modifiers)
{
	for (NSString *text in @[@"•", @"←", @"→"]) {
		for (int down = 0; down <= 1; down++) {
			CGEventRef event = symbolEvent(text, down);
			ASSERT(event != NULL);
			NSEvent *native = [NSEvent eventWithCGEvent:event];
			ASSERT_EQ(0, nativeModifiers(native));
			if (down) {
				ASSERT([native.characters isEqualToString:text]);
				ASSERT([native.charactersIgnoringModifiers isEqualToString:text]);
				ASSERT_EQ([text characterAtIndex:0], nativeKeysym(native));
			} else {
				ASSERT_EQ(0, native.characters.length);
				ASSERT_EQ(NoSymbol, nativeKeysym(native));
			}
			CFRelease(event);
		}
	}
}

TEST(kitty_tui_receives_unicode_not_function_or_control_keys)
{
	win.mode = MODE_KITTYKBD;
	altScreen = 1;
	NSArray *symbols = @[@"•", @"←", @"→"];
	const char *expected[] = {"\033[8226;1:1u", "\033[8592;1:1u", "\033[8594;1:1u"};
	for (int i = 0; i < 3; i++) {
		output[0] = '\0';
		CGEventRef event = symbolEvent(symbols[i], YES);
		nativeHandleKey([NSEvent eventWithCGEvent:event], 0);
		ASSERT_STR_EQ(expected[i], output);
		CFRelease(event);
		output[0] = '\0';
		event = symbolEvent(symbols[i], NO);
		nativeHandleKey([NSEvent eventWithCGEvent:event], 1);
		ASSERT_STR_EQ("", output);
		CFRelease(event);
	}
}

TEST(shell_unicode_reaches_text_input_instead_of_number_row_shortcuts)
{
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
	nativeView = [[STMetalView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)
	    device:MTLCreateSystemDefaultDevice()];
	win.mode = 0;
	altScreen = 0;
	for (NSString *text in @[@"•", @"←", @"→"]) {
		CGEventRef event = symbolEvent(text, YES);
		NSEvent *native = [NSEvent eventWithCGEvent:event];
		output[0] = '\0';
		nativeHandleKey(native, 0);
		ASSERT_STR_EQ(text.UTF8String, output);
		CFRelease(event);
	}
	nativeView = nil;
}

TEST(rejects_other_text)
{
	ASSERT(!validSymbol(@"a"));
	ASSERT(!validSymbol(@""));
	ASSERT(!validSymbol(@"•\n"));
}

TEST(targeted_delivery_to_this_hidden_test_process)
{
	/* A real WindowServer round trip, strictly to our own PID. */
	if (!CGPreflightPostEventAccess()) {
		puts("SKIP: event posting is not authorized");
		return;
	}
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
	win.mode = MODE_KITTYKBD;
	altScreen = 1;
	for (NSString *text in @[@"•", @"←", @"→"]) {
		CGEventRef event = symbolEvent(text, YES);
		CGEventPostToPid(getpid(), event);
		CFRelease(event);
		NSEvent *received = [NSApp nextEventMatchingMask:NSEventMaskKeyDown
		    untilDate:[NSDate dateWithTimeIntervalSinceNow:2]
		    inMode:NSDefaultRunLoopMode dequeue:YES];
		ASSERT(received != nil);
		ASSERT([received.characters isEqualToString:text]);
		ASSERT_EQ(0, nativeModifiers(received));
		output[0] = '\0';
		nativeHandleKey(received, 0);
		char expected[64];
		snprintf(expected, sizeof expected, "\033[%d;1:1u", [text characterAtIndex:0]);
		ASSERT_STR_EQ(expected, output);
		event = symbolEvent(text, NO);
		CGEventPostToPid(getpid(), event);
		CFRelease(event);
		received = [NSApp nextEventMatchingMask:NSEventMaskKeyUp
		    untilDate:[NSDate dateWithTimeIntervalSinceNow:2]
		    inMode:NSDefaultRunLoopMode dequeue:YES];
		ASSERT(received != nil);
		output[0] = '\0';
		nativeHandleKey(received, 1);
		ASSERT_STR_EQ("", output);
	}
}

TEST(short_lived_helper_delivers_before_exiting)
{
	if (!CGPreflightPostEventAccess()) {
		puts("SKIP: event posting is not authorized");
		return;
	}
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
	nativeView = [[STMetalView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)
	    device:MTLCreateSystemDefaultDevice()];
	NSString *sender = [NSFileManager.defaultManager.currentDirectoryPath
	    stringByAppendingPathComponent:@"tests/macos_symbol_sender"];
	for (NSString *text in @[@"•", @"←", @"→", @"•", @"←", @"→"]) {
		NSTask *child = [NSTask new];
		child.executableURL = [NSURL fileURLWithPath:sender];
		child.arguments = @[text];
		ASSERT([child launchAndReturnError:nil]);
		NSEvent *down = [NSApp nextEventMatchingMask:NSEventMaskKeyDown
		    untilDate:[NSDate dateWithTimeIntervalSinceNow:2]
		    inMode:NSDefaultRunLoopMode dequeue:YES];
		NSEvent *up = [NSApp nextEventMatchingMask:NSEventMaskKeyUp
		    untilDate:[NSDate dateWithTimeIntervalSinceNow:2]
		    inMode:NSDefaultRunLoopMode dequeue:YES];
		[child waitUntilExit];
		ASSERT_EQ(0, child.terminationStatus);
		ASSERT(down != nil);
		ASSERT(up != nil);
		ASSERT([down.characters isEqualToString:text]);
		ASSERT_EQ(0, nativeModifiers(down));
		win.mode = 0;
		output[0] = '\0';
		nativeHandleKey(down, 0);
		ASSERT_STR_EQ(text.UTF8String, output);
		win.mode = MODE_KITTYKBD;
		output[0] = '\0';
		nativeHandleKey(down, 0);
		char expected[64];
		snprintf(expected, sizeof expected, "\033[%d;1:1u", [text characterAtIndex:0]);
		ASSERT_STR_EQ(expected, output);
	}
	nativeView = nil;
}

int
main(void)
{
	@autoreleasepool {
		RUN_TEST(unicode_events_preserve_text_and_clear_modifiers);
		RUN_TEST(kitty_tui_receives_unicode_not_function_or_control_keys);
		RUN_TEST(shell_unicode_reaches_text_input_instead_of_number_row_shortcuts);
		RUN_TEST(rejects_other_text);
		RUN_TEST(targeted_delivery_to_this_hidden_test_process);
		RUN_TEST(short_lived_helper_delivers_before_exiting);
		return test_summary();
	}
}

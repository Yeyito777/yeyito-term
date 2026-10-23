/* Literal Unicode events, without System Events' keyboard-layout translation.
 * Build: clang -fobjc-arc -framework Cocoa macos-type-symbol.m -o type-symbol
 * No clipboard, focus changes, or permission prompts. */
#import <AppKit/AppKit.h>

static CGEventRef
traceSymbolEvent(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
    void *context)
{
	(void)proxy;
	/* Observe only this helper's tagged events, never other keyboard input. */
	if (event && CGEventGetIntegerValueField(event, kCGEventSourceUserData) == getpid()) {
		NSMutableArray *events = (__bridge NSMutableArray *)context;
		[events addObject:@{
		    @"type": @(type),
		    @"keycode": @(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)),
		    @"flags": @(CGEventGetFlags(event))
		}];
	}
	return event;
}

static BOOL
validSymbol(NSString *text)
{
	return [@[@"•", @"←", @"→"] containsObject:text];
}

static CGEventRef
symbolEvent(NSString *text, BOOL down)
{
	/* Private state avoids inheriting the physically held Control modifier. */
	CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
	if (!source) return NULL;
	/* An unmapped virtual key prevents Cocoa reconstructing an "a" key-up
	 * (key code 0) instead of the Unicode key-down in Kitty keyboard mode. */
	CGEventRef event = CGEventCreateKeyboardEvent(source, 0xff, down);
	CFRelease(source);
	if (!event) return NULL;
	UniChar character = [text characterAtIndex:0];
	CGEventKeyboardSetUnicodeString(event, 1, &character);
	CGEventSetFlags(event, 0);
	return event;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		BOOL trace = argc == 3 && !strcmp(argv[1], "--trace");
		if (argc != 2 && !trace) return 2;
		NSString *text = [NSString stringWithUTF8String:argv[trace ? 2 : 1]];
		if ([text isEqualToString:@"--probe"]) {
			NSRunningApplication *target = NSWorkspace.sharedWorkspace.frontmostApplication;
			NSDictionary *probe = @{
			    @"post_access": @(CGPreflightPostEventAccess()),
			    @"pid": @(getpid()), @"parent_pid": @(getppid()),
			    @"target_pid": @(target.processIdentifier),
			    @"target_bundle": target.bundleIdentifier ?: @"",
			    @"probe_only": @YES
			};
			NSData *json = [NSJSONSerialization dataWithJSONObject:probe options:0 error:nil];
			fwrite(json.bytes, 1, json.length, stdout);
			fputc('\n', stdout);
			return 0;
		}
		if (!validSymbol(text)) return 2;
		BOOL access = CGPreflightPostEventAccess();
		NSRunningApplication *target = NSWorkspace.sharedWorkspace.frontmostApplication;
		if (trace) {
			fprintf(stdout, "{\"invoked\":true,\"post_access\":%s,\"pid\":%d,\"target_pid\":%d,\"target_bundle\":\"%s\"}\n",
			    access ? "true" : "false", getpid(), target.processIdentifier,
			    target.bundleIdentifier.UTF8String ?: "");
			fflush(stdout);
		}
		if (!access) {
			fprintf(stderr, "type-symbol: Accessibility event access is unavailable\n");
			return 3;
		}
		if (!target || target.terminated) return 4;
		CGEventRef down = symbolEvent(text, YES);
		CGEventRef up = symbolEvent(text, NO);
		if (!down || !up) {
			if (down) CFRelease(down);
			if (up) CFRelease(up);
			return 5;
		}
		NSMutableArray *events = [NSMutableArray array];
		CFMachPortRef tap = trace ? CGEventTapCreateForPid(target.processIdentifier,
		    kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
		    CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp),
		    traceSymbolEvent, (__bridge void *)events) : NULL;
		CFRunLoopSourceRef tapSource = tap ?
		    CFMachPortCreateRunLoopSource(NULL, tap, 0) : NULL;
		if (tapSource)
			CFRunLoopAddSource(CFRunLoopGetCurrent(), tapSource, kCFRunLoopDefaultMode);
		if (trace) {
			CGEventSetIntegerValueField(down, kCGEventSourceUserData, getpid());
			CGEventSetIntegerValueField(up, kCGEventSourceUserData, getpid());
		}
		/* Deliver the pair only to the application selected at invocation;
		 * changing focus during the shortcut cannot redirect half the pair. */
		CGEventPostToPid(target.processIdentifier, down);
		CGEventPostToPid(target.processIdentifier, up);
		/* Posting is asynchronous: exiting here can discard both events before
		 * WindowServer hands them to the recipient. Service the sender's run
		 * loop for a bounded handoff interval, including in the non-trace path.
		 * A timer keeps the run loop alive even when it has no other sources. */
		CFRunLoopTimerRef handoff = CFRunLoopTimerCreateWithHandler(NULL,
		    CFAbsoluteTimeGetCurrent() + 0.5, 0, 0, 0, ^(CFRunLoopTimerRef timer) {
			(void)timer;
		});
		CFRunLoopAddTimer(CFRunLoopGetCurrent(), handoff, kCFRunLoopDefaultMode);
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
		CFRunLoopTimerInvalidate(handoff);
		CFRelease(handoff);
		CFRelease(down);
		CFRelease(up);
		if (trace) {
			NSDictionary *result = @{@"posted": @YES, @"tap_created": @(tap != NULL),
			    @"observed_at_target": events};
			NSData *json = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
			fwrite(json.bytes, 1, json.length, stdout);
			fputc('\n', stdout);
		}
		if (tapSource) {
			CFRunLoopRemoveSource(CFRunLoopGetCurrent(), tapSource, kCFRunLoopDefaultMode);
			CFRelease(tapSource);
		}
		if (tap) { CFMachPortInvalidate(tap); CFRelease(tap); }
	}
	return 0;
}

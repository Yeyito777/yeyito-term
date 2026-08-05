/* Regression tests for native macOS OSC 5522 clipboard transport. */
#import <AppKit/AppKit.h>

#include "test.h"
#include "../macos/pasteboard5522.h"

static void
capture(const char *data, size_t length, void *context)
{
	[(__bridge NSMutableData *)context appendBytes:data length:length];
}

static NSString *
capturedString(NSMutableData *captureData)
{
	return [[NSString alloc] initWithData:captureData
	    encoding:NSUTF8StringEncoding];
}

static NSString *
passwordFromOutput(NSString *output)
{
	NSRange start = [output rangeOfString:@":pw="];
	if (start.location == NSNotFound)
		return nil;
	NSUInteger offset = NSMaxRange(start);
	NSRange rest = NSMakeRange(offset, output.length - offset);
	NSRange end = [output rangeOfString:@"\033\\" options:0 range:rest];
	if (end.location == NSNotFound)
		return nil;
	return [output substringWithRange:NSMakeRange(offset,
	    end.location - offset)];
}

static BOOL
makeImageRequest(NSString *password, Clip5522Request *request)
{
	NSString *metadata = [NSString stringWithFormat:
	    @"type=read:name=UGFzdGUgZXZlbnQ=:pw=%@", password];
	return clip5522_parse_read(metadata.UTF8String,
	    "aW1hZ2UvcG5n", request);
}

TEST(image_paste_event_transfers_png_once)
{
	NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
	unsigned char bytes[] = {0x89, 'P', 'N', 'G'};
	[pasteboard clearContents];
	ASSERT([pasteboard setData:[NSData dataWithBytes:bytes length:sizeof(bytes)]
	    forType:NSPasteboardTypePNG]);
	NSMutableData *outputData = [NSMutableData data];
	STPasteboard5522 *transport = [[STPasteboard5522 alloc]
	    initWithPasteboard:pasteboard writer:capture
	    context:(__bridge void *)outputData];

	[transport beginPasteEvent];
	NSString *offer = capturedString(outputData);
	NSString *password = passwordFromOutput(offer);
	ASSERT_NOT_NULL(password);
	ASSERT([offer containsString:@"status=OK"]);
	ASSERT([offer containsString:@"mime=Lg=="]);
	ASSERT([offer containsString:@"aW1hZ2UvcG5n"]);

	Clip5522Request request;
	ASSERT(makeImageRequest(password, &request));
	[outputData setLength:0];
	[transport readRequest:&request];
	NSString *transfer = capturedString(outputData);
	ASSERT([transfer containsString:@"status=OK"]);
	ASSERT([transfer containsString:@"mime=aW1hZ2UvcG5n"]);
	ASSERT([transfer containsString:@";iVBORw==\033\\"]);
	ASSERT([transfer containsString:@"status=DONE"]);

	[outputData setLength:0];
	[transport readRequest:&request];
	ASSERT([capturedString(outputData) containsString:@"status=EPERM"]);
	clip5522_request_free(&request);
	[pasteboard releaseGlobally];
}

TEST(changed_pasteboard_invalidates_event)
{
	NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
	[pasteboard clearContents];
	ASSERT([pasteboard setData:[@"first" dataUsingEncoding:NSUTF8StringEncoding]
	    forType:NSPasteboardTypePNG]);
	NSMutableData *outputData = [NSMutableData data];
	STPasteboard5522 *transport = [[STPasteboard5522 alloc]
	    initWithPasteboard:pasteboard writer:capture
	    context:(__bridge void *)outputData];
	[transport beginPasteEvent];
	NSString *password = passwordFromOutput(capturedString(outputData));
	ASSERT_NOT_NULL(password);

	[pasteboard clearContents];
	ASSERT([pasteboard setData:[@"second" dataUsingEncoding:NSUTF8StringEncoding]
	    forType:NSPasteboardTypePNG]);
	Clip5522Request request;
	ASSERT(makeImageRequest(password, &request));
	[outputData setLength:0];
	[transport readRequest:&request];
	ASSERT([capturedString(outputData) containsString:@"status=EPERM"]);
	clip5522_request_free(&request);
	[pasteboard releaseGlobally];
}

TEST(mime_list_can_be_queried_without_clipboard_data)
{
	NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
	[pasteboard clearContents];
	ASSERT([pasteboard setString:@"hello" forType:NSPasteboardTypeString]);
	NSMutableData *outputData = [NSMutableData data];
	STPasteboard5522 *transport = [[STPasteboard5522 alloc]
	    initWithPasteboard:pasteboard writer:capture
	    context:(__bridge void *)outputData];
	Clip5522Request request;
	ASSERT(clip5522_parse_read("type=read", "Lg==", &request));
	[transport readRequest:&request];
	NSString *output = capturedString(outputData);
	ASSERT([output containsString:@"status=OK"]);
	ASSERT([output containsString:@"dGV4dC9wbGFpbg=="]);
	ASSERT([output containsString:@"status=DONE"]);
	ASSERT([output rangeOfString:@":pw="].location == NSNotFound);
	clip5522_request_free(&request);
	[pasteboard releaseGlobally];
}

TEST_SUITE(macos_pasteboard5522)
{
	RUN_TEST(image_paste_event_transfers_png_once);
	RUN_TEST(changed_pasteboard_invalidates_event);
	RUN_TEST(mime_list_can_be_queried_without_clipboard_data);
}

int
main(void)
{
	@autoreleasepool {
		RUN_SUITE(macos_pasteboard5522);
		return test_summary();
	}
}

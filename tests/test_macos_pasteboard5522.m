/* Regression tests for native macOS OSC 5522 clipboard transport. */
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

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


static Clip5522Request
mimeRequest(NSString *password, NSString *mime, BOOL *ok)
{
	Clip5522Request request;
	NSString *metadata = [NSString stringWithFormat:
	    @"type=read:name=UGFzdGUgZXZlbnQ=:pw=%@", password];
	NSString *encoded = [[mime dataUsingEncoding:NSUTF8StringEncoding]
	    base64EncodedStringWithOptions:0];
	*ok = clip5522_parse_read(metadata.UTF8String, encoded.UTF8String,
	    &request) ? YES : NO;
	return request;
}

/* The MIME list a paste event offers, in order. */
static NSArray<NSString *> *
offeredMIMEs(NSString *offer)
{
	/* A paste event's list packet is ...:mime=Lg==:pw=TOKEN;LIST ESC \ */
	NSRange mime = [offer rangeOfString:@"mime=Lg=="];
	if (mime.location == NSNotFound)
		return @[];
	NSRange start = [offer rangeOfString:@";" options:0
	    range:NSMakeRange(NSMaxRange(mime), offer.length - NSMaxRange(mime))];
	if (start.location == NSNotFound)
		return @[];
	NSUInteger offset = NSMaxRange(start);
	NSRange end = [offer rangeOfString:@"\033\\" options:0
	    range:NSMakeRange(offset, offer.length - offset)];
	if (end.location == NSNotFound)
		return @[];
	NSData *list = [[NSData alloc] initWithBase64EncodedString:
	    [offer substringWithRange:NSMakeRange(offset, end.location - offset)]
	    options:0];
	NSString *text = [[NSString alloc] initWithData:list
	    encoding:NSUTF8StringEncoding];
	return text.length ? [text componentsSeparatedByString:@" "] : @[];
}

/* Reassembles the DATA packets of one MIME from a transfer. */
static NSData *
transferredData(NSString *transfer, NSString *mime)
{
	NSString *encodedMime = [[mime dataUsingEncoding:NSUTF8StringEncoding]
	    base64EncodedStringWithOptions:0];
	NSString *marker = [NSString stringWithFormat:
	    @"status=DATA:mime=%@;", encodedMime];
	NSMutableData *bytes = [NSMutableData data];
	NSRange search = NSMakeRange(0, transfer.length);
	for (;;) {
		NSRange start = [transfer rangeOfString:marker options:0 range:search];
		if (start.location == NSNotFound)
			break;
		NSUInteger offset = NSMaxRange(start);
		NSRange end = [transfer rangeOfString:@"\033\\" options:0
		    range:NSMakeRange(offset, transfer.length - offset)];
		if (end.location == NSNotFound)
			return nil;
		NSData *chunk = [[NSData alloc] initWithBase64EncodedString:
		    [transfer substringWithRange:NSMakeRange(offset,
		    end.location - offset)] options:0];
		if (!chunk)
			return nil;
		[bytes appendData:chunk];
		search = NSMakeRange(NSMaxRange(end), transfer.length - NSMaxRange(end));
	}
	return bytes.length ? bytes : nil;
}

/* A solid image, wider than tall, written with ImageIO. */
static BOOL
writeImage(NSURL *url, CFStringRef type, size_t width, size_t height,
	BOOL alpha, int orientation)
{
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, 0,
	    space, (CGBitmapInfo)(alpha ? kCGImageAlphaPremultipliedLast :
	    kCGImageAlphaNoneSkipLast));
	CGColorSpaceRelease(space);
	if (!context)
		return NO;
	CGContextSetRGBFillColor(context, 0.9, 0.1, 0.1, alpha ? 0.5 : 1);
	CGContextFillRect(context, CGRectMake(0, 0, width, height));
	CGImageRef image = CGBitmapContextCreateImage(context);
	CGContextRelease(context);
	CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
	    (__bridge CFURLRef)url, type, 1, NULL);
	BOOL ok = NO;
	if (destination && image) {
		NSDictionary *properties = @{
			(__bridge NSString *)kCGImagePropertyOrientation: @(orientation),
		};
		CGImageDestinationAddImage(destination, image,
		    (__bridge CFDictionaryRef)properties);
		ok = CGImageDestinationFinalize(destination);
	}
	if (destination)
		CFRelease(destination);
	CGImageRelease(image);
	return ok;
}

static NSURL *
tempURL(NSString *name)
{
	NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:
	    [NSString stringWithFormat:@"st-pasteboard5522-%d", getpid()]];
	[NSFileManager.defaultManager createDirectoryAtPath:dir
	    withIntermediateDirectories:YES attributes:nil error:NULL];
	return [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:name]];
}

/* What Finder's Copy leaves: file URLs, their names, and an icon TIFF. */
static NSData *
finderIconTIFF(void)
{
	NSBitmapImageRep *icon = [[NSBitmapImageRep alloc]
	    initWithBitmapDataPlanes:NULL pixelsWide:16 pixelsHigh:16
	    bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
	    colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
	return icon.TIFFRepresentation;
}

static NSPasteboard *
finderPasteboard(NSArray<NSURL *> *urls)
{
	NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
	[pasteboard clearContents];
	[pasteboard writeObjects:urls];
	NSMutableArray<NSString *> *names = [NSMutableArray array];
	for (NSURL *url in urls)
		[names addObject:url.lastPathComponent];
	[pasteboard setString:[names componentsJoinedByString:@"\r"]
	    forType:NSPasteboardTypeString];
	[pasteboard setData:finderIconTIFF() forType:NSPasteboardTypeTIFF];
	return pasteboard;
}

typedef struct {
	NSArray<NSString *> *mimes;
	NSData *data;    /* the first offered MIME's payload, nil on failure */
	NSString *reply; /* the read's raw reply */
} PasteResult;

static PasteResult
pasteFirstMIME(NSPasteboard *pasteboard)
{
	PasteResult result = {@[], nil, @""};
	NSMutableData *outputData = [NSMutableData data];
	STPasteboard5522 *transport = [[STPasteboard5522 alloc]
	    initWithPasteboard:pasteboard writer:capture
	    context:(__bridge void *)outputData];
	[transport beginPasteEvent];
	NSString *offer = capturedString(outputData);
	result.mimes = offeredMIMEs(offer);
	NSString *password = passwordFromOutput(offer);
	if (!password || !result.mimes.count)
		return result;
	BOOL ok = NO;
	NSString *first = result.mimes.firstObject;
	Clip5522Request request = mimeRequest(password, first, &ok);
	if (!ok)
		return result;
	[outputData setLength:0];
	[transport readRequest:&request];
	clip5522_request_free(&request);
	result.reply = capturedString(outputData);
	result.data = transferredData(result.reply, first);
	return result;
}

static CGSize
decodedSize(NSData *data)
{
	CGImageSourceRef source = CGImageSourceCreateWithData(
	    (__bridge CFDataRef)data, NULL);
	if (!source)
		return CGSizeZero;
	NSDictionary *properties = CFBridgingRelease(
	    CGImageSourceCopyPropertiesAtIndex(source, 0, NULL));
	CFRelease(source);
	return CGSizeMake(
	    [properties[(__bridge NSString *)kCGImagePropertyPixelWidth] doubleValue],
	    [properties[(__bridge NSString *)kCGImagePropertyPixelHeight] doubleValue]);
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

TEST(finder_copied_png_pastes_the_file_not_its_icon)
{
	NSURL *url = tempURL(@"IMG_1198.PNG");
	ASSERT(writeImage(url, CFSTR("public.png"), 40, 30, NO, 1));
	NSData *file = [NSData dataWithContentsOfURL:url];
	PasteResult result = pasteFirstMIME(finderPasteboard(@[url]));
	ASSERT([result.mimes isEqualToArray:(@[@"image/png", @"text/plain"])]);
	ASSERT([result.data isEqualToData:file]);
}

TEST(finder_copied_heic_photo_is_transcoded_upright)
{
	NSURL *url = tempURL(@"IMG_2000.HEIC");
	/* Orientation 6: stored landscape, displayed portrait. */
	if (!writeImage(url, CFSTR("public.heic"), 64, 32, NO, 6)) {
		printf("  (no HEIC encoder; skipped)\n");
		return;
	}
	PasteResult result = pasteFirstMIME(finderPasteboard(@[url]));
	ASSERT([result.mimes isEqualToArray:(@[@"image/jpeg", @"text/plain"])]);
	ASSERT_NOT_NULL(result.data);
	const unsigned char *bytes = result.data.bytes;
	ASSERT(result.data.length > 4 && bytes[0] == 0xff && bytes[1] == 0xd8);
	CGSize size = decodedSize(result.data);
	ASSERT(size.width == 32 && size.height == 64);
}

TEST(rotated_tiff_with_alpha_becomes_upright_png)
{
	NSURL *url = tempURL(@"scan.tiff");
	ASSERT(writeImage(url, CFSTR("public.tiff"), 50, 20, YES, 8));
	PasteResult result = pasteFirstMIME(finderPasteboard(@[url]));
	ASSERT([result.mimes.firstObject isEqualToString:@"image/png"]);
	ASSERT_NOT_NULL(result.data);
	CGSize size = decodedSize(result.data);
	ASSERT(size.width == 20 && size.height == 50);
}

TEST(oversized_photo_is_fitted)
{
	NSURL *url = tempURL(@"huge.jpg");
	ASSERT(writeImage(url, CFSTR("public.jpeg"), 9000, 3000, NO, 1));
	PasteResult result = pasteFirstMIME(finderPasteboard(@[url]));
	ASSERT([result.mimes.firstObject isEqualToString:@"image/jpeg"]);
	ASSERT_NOT_NULL(result.data);
	ASSERT(result.data.length <= 3U * 1024U * 1024U);
	CGSize size = decodedSize(result.data);
	ASSERT(size.width == 2576 && size.height < 900);
}

TEST(unreadable_image_file_fails_instead_of_sending_icon)
{
	NSURL *url = tempURL(@"broken.heic");
	ASSERT([[@"not a photo" dataUsingEncoding:NSUTF8StringEncoding]
	    writeToURL:url atomically:YES]);
	PasteResult result = pasteFirstMIME(finderPasteboard(@[url]));
	ASSERT([result.mimes.firstObject isEqualToString:@"image/png"]);
	ASSERT(result.data == nil);
	ASSERT([result.reply containsString:@"status=ENOSYS"]);
}

TEST(several_files_with_an_image_never_send_icon)
{
	NSURL *image = tempURL(@"a.png");
	NSURL *other = tempURL(@"b.png");
	ASSERT(writeImage(image, CFSTR("public.png"), 8, 8, NO, 1));
	ASSERT(writeImage(other, CFSTR("public.png"), 8, 8, NO, 1));
	PasteResult result = pasteFirstMIME(finderPasteboard(@[image, other]));
	ASSERT([result.mimes.firstObject isEqualToString:@"image/png"]);
	ASSERT(result.data == nil);
	ASSERT([result.reply containsString:@"status=ENOSYS"]);
}

TEST(non_image_file_keeps_previous_offer)
{
	NSURL *url = tempURL(@"notes.txt");
	ASSERT([[@"hello" dataUsingEncoding:NSUTF8StringEncoding]
	    writeToURL:url atomically:YES]);
	PasteResult result = pasteFirstMIME(finderPasteboard(@[url]));
	ASSERT([result.mimes isEqualToArray:(@[@"image/png", @"text/plain"])]);
	CGSize size = decodedSize(result.data);
	ASSERT(size.width == 16 && size.height == 16);
}

TEST_SUITE(macos_pasteboard5522)
{
	RUN_TEST(image_paste_event_transfers_png_once);
	RUN_TEST(changed_pasteboard_invalidates_event);
	RUN_TEST(mime_list_can_be_queried_without_clipboard_data);
	RUN_TEST(finder_copied_png_pastes_the_file_not_its_icon);
	RUN_TEST(finder_copied_heic_photo_is_transcoded_upright);
	RUN_TEST(rotated_tiff_with_alpha_becomes_upright_png);
	RUN_TEST(oversized_photo_is_fitted);
	RUN_TEST(unreadable_image_file_fails_instead_of_sending_icon);
	RUN_TEST(several_files_with_an_image_never_send_icon);
	RUN_TEST(non_image_file_keeps_previous_offer);
}

int
main(void)
{
	@autoreleasepool {
		RUN_SUITE(macos_pasteboard5522);
		return test_summary();
	}
}

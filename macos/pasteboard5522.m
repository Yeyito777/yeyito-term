/* Native macOS clipboard transport for Kitty's OSC 5522 protocol. */
#import "pasteboard5522.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLIP5522_MAX_BYTES (32U * 1024U * 1024U)
#define CLIP5522_TOKEN_SECONDS 15

static NSPasteboardType const STPasteboardTypeJPEG = @"public.jpeg";
static NSPasteboardType const STPasteboardTypeGIF = @"com.compuserve.gif";
static NSPasteboardType const STPasteboardTypeWebP = @"org.webmproject.webp";
static NSPasteboardType const STPasteboardTypePublicWebP = @"public.webp";

@implementation STPasteboard5522 {
	NSPasteboard *_pasteboard;
	Clip5522Write _writer;
	void *_context;
	NSString *_token;
	NSInteger _changeCount;
	struct timespec _expiry;
}

- (instancetype)initWithPasteboard:(NSPasteboard *)pasteboard
	writer:(Clip5522Write)writer context:(void *)context
{
	self = [super init];
	if (self) {
		_pasteboard = pasteboard;
		_writer = writer;
		_context = context;
	}
	return self;
}

- (void)emitStatus:(const char *)status password:(const char *)password
{
	clip5522_status(_writer, _context, status, 0, password);
}

- (NSArray<NSString *> *)availableMIMETypes
{
	NSMutableArray<NSString *> *mimes = [NSMutableArray arrayWithCapacity:5];
	if ([_pasteboard availableTypeFromArray:@[NSPasteboardTypePNG,
	    NSPasteboardTypeTIFF]])
		[mimes addObject:@"image/png"];
	if ([_pasteboard availableTypeFromArray:@[STPasteboardTypeJPEG]])
		[mimes addObject:@"image/jpeg"];
	if ([_pasteboard availableTypeFromArray:@[STPasteboardTypeGIF]])
		[mimes addObject:@"image/gif"];
	if ([_pasteboard availableTypeFromArray:@[STPasteboardTypeWebP,
	    STPasteboardTypePublicWebP]])
		[mimes addObject:@"image/webp"];
	if ([_pasteboard availableTypeFromArray:@[NSPasteboardTypeString]])
		[mimes addObject:@"text/plain"];
	return mimes;
}

- (NSData *)dataForMIME:(NSString *)mime
{
	if ([mime isEqualToString:@"image/png"]) {
		NSData *data = [_pasteboard dataForType:NSPasteboardTypePNG];
		if (data)
			return data;
		NSData *tiff = [_pasteboard dataForType:NSPasteboardTypeTIFF];
		NSBitmapImageRep *bitmap = tiff ?
		    [NSBitmapImageRep imageRepWithData:tiff] : nil;
		return [bitmap representationUsingType:NSBitmapImageFileTypePNG
		    properties:@{}];
	}
	if ([mime isEqualToString:@"image/jpeg"])
		return [_pasteboard dataForType:STPasteboardTypeJPEG];
	if ([mime isEqualToString:@"image/gif"])
		return [_pasteboard dataForType:STPasteboardTypeGIF];
	if ([mime isEqualToString:@"image/webp"]) {
		NSPasteboardType type = [_pasteboard availableTypeFromArray:@[
		    STPasteboardTypeWebP, STPasteboardTypePublicWebP]];
		return type ? [_pasteboard dataForType:type] : nil;
	}
	if ([mime isEqualToString:@"text/plain"]) {
		NSString *string = [_pasteboard stringForType:NSPasteboardTypeString];
		return [string dataUsingEncoding:NSUTF8StringEncoding];
	}
	return nil;
}

- (void)sendMIMEList:(NSArray<NSString *> *)mimes password:(const char *)password
{
	NSString *list = [mimes componentsJoinedByString:@" "];
	[self emitStatus:"OK" password:password];
	clip5522_data(_writer, _context, ".",
	    (const unsigned char *)list.UTF8String,
	    [list lengthOfBytesUsingEncoding:NSUTF8StringEncoding], password);
	[self emitStatus:"DONE" password:password];
}

- (BOOL)makeToken
{
	unsigned char raw[24];
	char *encoded = NULL;
	arc4random_buf(raw, sizeof(raw));
	if (!clip5522_base64(raw, sizeof(raw), &encoded))
		return NO;
	_token = [NSString stringWithUTF8String:encoded];
	free(encoded);
	if (!_token)
		return NO;
	_changeCount = _pasteboard.changeCount;
	clock_gettime(CLOCK_MONOTONIC, &_expiry);
	_expiry.tv_sec += CLIP5522_TOKEN_SECONDS;
	return YES;
}

- (BOOL)tokenIsValidForRequest:(const Clip5522Request *)request
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	BOOL expired = now.tv_sec > _expiry.tv_sec ||
	    (now.tv_sec == _expiry.tv_sec && now.tv_nsec > _expiry.tv_nsec);
	return request->password && request->paste_event_name && !request->primary &&
	    _token && !expired && _changeCount == _pasteboard.changeCount &&
	    !strcmp(request->password, _token.UTF8String);
}

- (void)beginPasteEvent
{
	_token = nil;
	NSArray<NSString *> *mimes = [self availableMIMETypes];
	if (![self makeToken]) {
		[self emitStatus:"EIO" password:NULL];
		return;
	}
	[self sendMIMEList:mimes password:_token.UTF8String];
}

- (void)readRequest:(const Clip5522Request *)request
{
	if (request->nmimes == 1 && !strcmp(request->mimes[0], ".")) {
		if (request->primary) {
			[self emitStatus:"ENOSYS" password:NULL];
			return;
		}
		[self sendMIMEList:[self availableMIMETypes] password:NULL];
		return;
	}
	if (![self tokenIsValidForRequest:request]) {
		[self emitStatus:"EPERM" password:NULL];
		return;
	}

	/* Paste-event passwords are single-use even when a conversion fails. */
	_token = nil;
	NSMutableArray<NSString *> *mimes = [NSMutableArray array];
	NSMutableArray<NSData *> *payloads = [NSMutableArray array];
	size_t total = 0;
	for (size_t i = 0; i < request->nmimes; i++) {
		NSString *mime = [NSString stringWithUTF8String:request->mimes[i]];
		NSData *data = mime ? [self dataForMIME:mime] : nil;
		if (!data)
			continue;
		if (data.length > CLIP5522_MAX_BYTES - total) {
			[self emitStatus:"EIO" password:NULL];
			return;
		}
		total += data.length;
		[mimes addObject:mime];
		[payloads addObject:data];
	}
	if (!payloads.count) {
		[self emitStatus:"ENOSYS" password:NULL];
		return;
	}

	[self emitStatus:"OK" password:NULL];
	for (NSUInteger i = 0; i < payloads.count; i++) {
		NSData *data = payloads[i];
		clip5522_data(_writer, _context, mimes[i].UTF8String,
		    data.bytes, data.length, NULL);
	}
	[self emitStatus:"DONE" password:NULL];
}

- (void)invalidate
{
	_token = nil;
}

@end

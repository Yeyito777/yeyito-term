/* Native macOS clipboard transport for Kitty's OSC 5522 protocol. */
#import "pasteboard5522.h"
#import <ImageIO/ImageIO.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLIP5522_MAX_BYTES (32U * 1024U * 1024U)
#define CLIP5522_TOKEN_SECONDS 15

static NSPasteboardType const STPasteboardTypeJPEG = @"public.jpeg";
static NSPasteboardType const STPasteboardTypeGIF = @"com.compuserve.gif";
static NSPasteboardType const STPasteboardTypeWebP = @"org.webmproject.webp";
static NSPasteboardType const STPasteboardTypePublicWebP = @"public.webp";

/*
 * A file copied in Finder puts its URL on the pasteboard beside a TIFF of its
 * icon. When that file is an image, the file's own pixels are the paste:
 * PNG/JPEG/GIF/WebP go as they are while they fit; anything else (HEIC, TIFF,
 * rotated or oversized photos) is decoded with its EXIF orientation applied
 * and re-encoded as JPEG, or PNG when it has alpha. The icon is never offered
 * for an image file, so a failed decode fails the read instead.
 */
#define CLIP5522_IMAGE_PASSTHROUGH_BYTES (3U * 1024U * 1024U)
#define CLIP5522_IMAGE_PASSTHROUGH_SIDE 8000
#define CLIP5522_IMAGE_MAX_SIDE 2576
#define CLIP5522_IMAGE_MIN_SIDE 256

typedef struct {
	NSURL *url;     /* nil: no image file on the pasteboard */
	NSString *mime; /* what the paste offers for it */
	BOOL passthrough;
} STFileImage;

static NSString *
passthroughMIME(NSString *uti)
{
	NSDictionary<NSString *, NSString *> *types = @{
		@"public.png": @"image/png",
		@"public.jpeg": @"image/jpeg",
		@"com.compuserve.gif": @"image/gif",
		@"org.webmproject.webp": @"image/webp",
		@"public.webp": @"image/webp",
	};
	return uti ? types[uti] : nil;
}

static BOOL
hasImageExtension(NSURL *url)
{
	static NSSet<NSString *> *extensions;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		extensions = [NSSet setWithArray:@[@"png", @"jpg", @"jpeg",
		    @"jpe", @"gif", @"webp", @"heic", @"heif", @"hif", @"avif",
		    @"tif", @"tiff", @"bmp", @"jp2", @"dng"]];
	});
	return [extensions containsObject:url.pathExtension.lowercaseString];
}

static NSNumber *
imageProperty(NSDictionary *properties, CFStringRef key)
{
	id value = properties[(__bridge NSString *)key];
	return [value isKindOfClass:NSNumber.class] ? value : nil;
}

/* Decodes the first image with orientation applied, fitted to maxSide. */
static NSData *
encodeImage(CGImageSourceRef source, NSString *mime, NSUInteger maxSide)
{
	NSDictionary *options = @{
		(__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
		(__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform: @YES,
		(__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize: @(maxSide),
		(__bridge NSString *)kCGImageSourceShouldCacheImmediately: @YES,
	};
	CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source,
	    CGImageSourceGetPrimaryImageIndex(source),
	    (__bridge CFDictionaryRef)options);
	if (!image)
		return nil;
	NSMutableData *data = [NSMutableData data];
	CFStringRef type = [mime isEqualToString:@"image/png"] ?
	    CFSTR("public.png") : CFSTR("public.jpeg");
	CGImageDestinationRef destination = CGImageDestinationCreateWithData(
	    (__bridge CFMutableDataRef)data, type, 1, NULL);
	BOOL ok = NO;
	if (destination) {
		NSDictionary *properties = @{
			(__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @0.85,
		};
		CGImageDestinationAddImage(destination, image,
		    (__bridge CFDictionaryRef)properties);
		ok = CGImageDestinationFinalize(destination);
		CFRelease(destination);
	}
	CGImageRelease(image);
	return ok ? data : nil;
}

static NSData *
fileImageData(STFileImage plan)
{
	if (!plan.url)
		return nil;
	if (plan.passthrough)
		return [NSData dataWithContentsOfURL:plan.url];
	CGImageSourceRef source = CGImageSourceCreateWithURL(
	    (__bridge CFURLRef)plan.url, NULL);
	if (!source)
		return nil;
	NSData *data = nil;
	/* Shrink until it fits; a screenshot as PNG can need a smaller box. */
	for (NSUInteger side = CLIP5522_IMAGE_MAX_SIDE;
	    !data && side >= CLIP5522_IMAGE_MIN_SIDE; side = side * 3 / 4) {
		NSData *encoded = encodeImage(source, plan.mime, side);
		if (!encoded)
			break;
		if (encoded.length <= CLIP5522_IMAGE_PASSTHROUGH_BYTES)
			data = encoded;
	}
	CFRelease(source);
	return data;
}

static STFileImage
planFileImage(NSURL *url)
{
	STFileImage none = {nil, nil, NO};
	STFileImage plan = {url.filePathURL, @"image/png", NO};
	if (!plan.url)
		return none;
	CGImageSourceRef source = CGImageSourceCreateWithURL(
	    (__bridge CFURLRef)plan.url, NULL);
	if (!source || CGImageSourceGetCount(source) < 1) {
		if (source)
			CFRelease(source);
		/* Named like an image but unreadable: fail the read, not the icon. */
		return hasImageExtension(plan.url) ? plan : none;
	}
	NSString *uti = [(__bridge NSString *)CGImageSourceGetType(source) copy];
	NSDictionary *properties = CFBridgingRelease(
	    CGImageSourceCopyPropertiesAtIndex(source,
	    CGImageSourceGetPrimaryImageIndex(source), NULL));
	CFRelease(source);
	NSNumber *width = imageProperty(properties, kCGImagePropertyPixelWidth);
	NSNumber *height = imageProperty(properties, kCGImagePropertyPixelHeight);
	NSNumber *orientation = imageProperty(properties,
	    kCGImagePropertyOrientation);
	NSNumber *alpha = imageProperty(properties, kCGImagePropertyHasAlpha);
	NSNumber *size = nil;
	[plan.url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL];
	NSString *mime = passthroughMIME(uti);
	if (mime && size && size.unsignedLongLongValue <= CLIP5522_IMAGE_PASSTHROUGH_BYTES &&
	    width && height && width.unsignedIntegerValue <= CLIP5522_IMAGE_PASSTHROUGH_SIDE &&
	    height.unsignedIntegerValue <= CLIP5522_IMAGE_PASSTHROUGH_SIDE &&
	    (!orientation || orientation.intValue == 1)) {
		plan.mime = mime;
		plan.passthrough = YES;
		return plan;
	}
	plan.mime = alpha.boolValue ? @"image/png" : @"image/jpeg";
	return plan;
}

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

/*
 * The image file a paste stands for. A file list holding an image but not
 * exactly one file plans an unreadable image, so the paste fails loudly
 * instead of sending the icon.
 */
- (STFileImage)fileImage
{
	STFileImage none = {nil, nil, NO};
	NSArray<NSURL *> *urls = [_pasteboard readObjectsForClasses:@[NSURL.class]
	    options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
	if (!urls.count)
		return none;
	if (urls.count == 1)
		return planFileImage(urls[0]);
	for (NSURL *url in urls) {
		if (planFileImage(url).mime) {
			STFileImage unreadable = {nil, @"image/png", NO};
			return unreadable;
		}
	}
	return none;
}

- (NSArray<NSString *> *)availableMIMETypes
{
	NSMutableArray<NSString *> *mimes = [NSMutableArray arrayWithCapacity:5];
	STFileImage file = [self fileImage];
	if (file.mime) {
		[mimes addObject:file.mime];
		if ([_pasteboard availableTypeFromArray:@[NSPasteboardTypeString]])
			[mimes addObject:@"text/plain"];
		return mimes;
	}
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
	STFileImage file = [self fileImage];
	if (file.mime && [mime hasPrefix:@"image/"])
		return [mime isEqualToString:file.mime] ? fileImageData(file) : nil;
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

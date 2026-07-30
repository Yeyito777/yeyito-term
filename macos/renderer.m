#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <simd/simd.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "glyph_layout.h"
#include "renderer.h"

#define ATLAS_SIZE 4096
#define GLYPH_PAD 2

typedef struct {
	vector_float2 position;
	vector_float2 uv;
	vector_float4 color;
	float mode;
} MacVertex;

typedef struct {
	MacVertex *items;
	size_t count;
	size_t capacity;
} MacVertexList;

@interface STGlyph : NSObject
@property(nonatomic) float u0, v0, u1, v1;
@property(nonatomic) float left, top, width, height, advance;
@property(nonatomic) float inkWidth, inkHeight;
@property(nonatomic) BOOL colorGlyph;
@end

@implementation STGlyph
@end

typedef struct {
	__unsafe_unretained MTKView *view;
	__strong id<MTLDevice> device;
	__strong id<MTLCommandQueue> queue;
	__strong id<MTLRenderPipelineState> pipeline;
	__strong id<MTLTexture> atlas;
	__strong id<MTLSamplerState> sampler;
	__strong NSMutableDictionary<NSString *, STGlyph *> *glyphs;
	CTFontRef fonts[4];
	char family[256];
	double pointSize;
	double scale;
	double ascent;
	double descent;
	double leading;
	double cellWidth;
	double cellHeight;
	NSUInteger atlasX, atlasY, atlasRowHeight;
	MacVertexList layers[MAC_LAYER_COUNT];
	__strong id<CAMetalDrawable> drawable;
	__strong MTLRenderPassDescriptor *pass;
	__strong id<MTLCommandBuffer> command;
} MacRenderer;

static MacRenderer r;

static NSString *const shaderSource = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct V { float2 position; float2 uv; float4 color; float mode; };\n"
"struct O { float4 position [[position]]; float2 uv; float4 color; float mode; };\n"
"vertex O st_vertex(const device V *v [[buffer(0)]], constant float2 &viewport [[buffer(1)]], uint id [[vertex_id]]) {\n"
"  O o; float2 p = v[id].position / viewport;\n"
"  o.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
"  o.uv = v[id].uv; o.color = v[id].color; o.mode = v[id].mode; return o;\n"
"}\n"
"fragment float4 st_fragment(O in [[stage_in]], texture2d<float> atlas [[texture(0)]], sampler samp [[sampler(0)]]) {\n"
"  if (in.mode < 0.5) return in.color;\n"
"  float4 s = atlas.sample(samp, in.uv);\n"
"  if (in.mode < 1.5) return float4(in.color.rgb, in.color.a * s.a);\n"
"  if (s.a > 0.0001) s.rgb /= s.a;\n"
"  return float4(s.rgb, s.a * in.color.a);\n"
"}\n";

static void
registerTerminalFonts(void)
{
	static int registered;
	if (registered)
		return;
	registered = 1;
	NSURL *terminal = [NSWorkspace.sharedWorkspace
	    URLForApplicationWithBundleIdentifier:@"com.apple.Terminal"];
	if (!terminal)
		terminal = [NSURL fileURLWithPath:
		    @"/System/Applications/Utilities/Terminal.app"];
	NSURL *directory = [terminal URLByAppendingPathComponent:
	    @"Contents/Resources/Fonts" isDirectory:YES];
	NSArray<NSURL *> *fonts = [NSFileManager.defaultManager
	    contentsOfDirectoryAtURL:directory includingPropertiesForKeys:nil
	    options:0 error:nil];
	for (NSURL *url in fonts) {
		if (![url.pathExtension.lowercaseString isEqualToString:@"otf"])
			continue;
		CFErrorRef error = NULL;
		CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,
		    kCTFontManagerScopeProcess, &error);
		if (error)
			CFRelease(error);
	}
}

static vector_float4
vcolor(MacColor c)
{
	return (vector_float4){c.r, c.g, c.b, c.a};
}

static void
listreset(MacVertexList *list)
{
	list->count = 0;
}

static void
listappend(MacVertexList *list, MacVertex vertex)
{
	if (list->count == list->capacity) {
		list->capacity = list->capacity ? list->capacity * 2 : 4096;
		list->items = realloc(list->items,
		    list->capacity * sizeof(*list->items));
		if (!list->items)
			abort();
	}
	list->items[list->count++] = vertex;
}

static void
quad(enum MacRenderLayer layer, float x, float y, float width, float height,
		float u0, float v0, float u1, float v1, MacColor color,
		float mode)
{
	MacVertexList *list = &r.layers[layer];
	vector_float4 c = vcolor(color);
	MacVertex a = {{x, y}, {u0, v0}, c, mode};
	MacVertex b = {{x + width, y}, {u1, v0}, c, mode};
	MacVertex d = {{x, y + height}, {u0, v1}, c, mode};
	MacVertex e = {{x + width, y + height}, {u1, v1}, c, mode};
	listappend(list, a); listappend(list, d); listappend(list, b);
	listappend(list, b); listappend(list, d); listappend(list, e);
}

static NSString *
fontFamily(const char *description)
{
	if (!description || !*description)
		return @"Menlo";
	NSString *value = [NSString stringWithUTF8String:description];
	NSRange colon = [value rangeOfString:@":"];
	if (colon.location != NSNotFound)
		value = [value substringToIndex:colon.location];
	return value.length ? value : @"Menlo";
}

static void
releaseFonts(void)
{
	for (int i = 0; i < 4; i++) {
		if (r.fonts[i]) {
			CFRelease(r.fonts[i]);
			r.fonts[i] = NULL;
		}
	}
}

static void
loadFonts(void)
{
	releaseFonts();
	registerTerminalFonts();
	NSString *family = fontFamily(r.family);
	CGFloat size = MAX(1.0, r.pointSize * r.scale);
	r.fonts[0] = CTFontCreateWithName((__bridge CFStringRef)family, size, NULL);
	if ([family isEqualToString:@"SFMono-Regular"]) {
		CFStringRef name = CTFontCopyPostScriptName(r.fonts[0]);
		BOOL exact = [(__bridge NSString *)name isEqualToString:family];
		CFRelease(name);
		if (!exact) {
			CFRelease(r.fonts[0]);
			r.fonts[0] = NULL;
		}
	}
	if (!r.fonts[0])
		r.fonts[0] = CTFontCreateWithName(CFSTR("Menlo"), size, NULL);

	CTFontSymbolicTraits traits[3] = {
		kCTFontBoldTrait, kCTFontItalicTrait,
		kCTFontBoldTrait | kCTFontItalicTrait
	};
	for (int i = 0; i < 3; i++) {
		r.fonts[i + 1] = CTFontCreateCopyWithSymbolicTraits(r.fonts[0],
		    size, NULL, traits[i], traits[i]);
		if (!r.fonts[i + 1])
			r.fonts[i + 1] = CFRetain(r.fonts[0]);
	}

	r.ascent = CTFontGetAscent(r.fonts[0]) / r.scale;
	r.descent = CTFontGetDescent(r.fonts[0]) / r.scale;
	r.leading = CTFontGetLeading(r.fonts[0]) / r.scale;
	UniChar sample = 'M';
	CGGlyph glyph = 0;
	CGSize advance = CGSizeZero;
	if (CTFontGetGlyphsForCharacters(r.fonts[0], &sample, &glyph, 1))
		CTFontGetAdvancesForGlyphs(r.fonts[0], kCTFontOrientationHorizontal,
		    &glyph, &advance, 1);
	r.cellWidth = MAX(1.0, advance.width / r.scale);
	r.cellHeight = MAX(1.0, r.ascent + r.descent + r.leading);

	[r.glyphs removeAllObjects];
	r.atlasX = r.atlasY = 1;
	r.atlasRowHeight = 0;
}

static void
createAtlas(void)
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	    width:ATLAS_SIZE height:ATLAS_SIZE mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	r.atlas = [r.device newTextureWithDescriptor:desc];
	r.atlas.label = @"st CoreText glyph atlas";
	r.atlasX = r.atlasY = 1;
	r.atlasRowHeight = 0;
}

static CTFontRef
styledFont(unsigned int style)
{
	int index = 0;
	if ((style & MAC_FONT_BOLD) && (style & MAC_FONT_ITALIC))
		index = 3;
	else if (style & MAC_FONT_BOLD)
		index = 1;
	else if (style & MAC_FONT_ITALIC)
		index = 2;
	return r.fonts[index] ?: r.fonts[0];
}

static CTLineRef
lineForString(NSString *string, CTFontRef font)
{
	NSDictionary *attrs = @{
		(__bridge NSString *)kCTFontAttributeName: (__bridge id)font,
		(__bridge NSString *)kCTLigatureAttributeName: @0
	};
	NSAttributedString *value = [[NSAttributedString alloc]
	    initWithString:string attributes:attrs];
	return CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)value);
}

static STGlyph *
glyphEntry(CTFontRef font, CGGlyph glyph)
{
	CFStringRef ps = CTFontCopyPostScriptName(font);
	NSString *key = [NSString stringWithFormat:@"%@:%u:%.3f",
	    (__bridge NSString *)ps, glyph, CTFontGetSize(font)];
	CFRelease(ps);
	STGlyph *cached = r.glyphs[key];
	if (cached)
		return cached;

	CGRect bounds = CTFontGetBoundingRectsForGlyphs(font,
	    kCTFontOrientationHorizontal, &glyph, NULL, 1);
	CGSize advance = CGSizeZero;
	CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal,
	    &glyph, &advance, 1);

	STGlyph *entry = [STGlyph new];
	entry.advance = advance.width;
	if (CGRectIsEmpty(bounds) || bounds.size.width <= 0 ||
	    bounds.size.height <= 0) {
		r.glyphs[key] = entry;
		return entry;
	}

	NSUInteger width = (NSUInteger)ceil(bounds.size.width) + 2 * GLYPH_PAD;
	NSUInteger height = (NSUInteger)ceil(bounds.size.height) + 2 * GLYPH_PAD;
	if (width > 512 || height > 512) {
		r.glyphs[key] = entry;
		return entry;
	}
	if (r.atlasX + width + 1 >= ATLAS_SIZE) {
		r.atlasX = 1;
		r.atlasY += r.atlasRowHeight + 1;
		r.atlasRowHeight = 0;
	}
	if (r.atlasY + height + 1 >= ATLAS_SIZE) {
		fprintf(stderr, "st: Metal glyph atlas is full\n");
		r.glyphs[key] = entry;
		return entry;
	}

	size_t bytesPerRow = width * 4;
	uint8_t *pixels = calloc(height, bytesPerRow);
	CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8,
	    bytesPerRow, colorspace,
	    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(colorspace);
	if (!context) {
		free(pixels);
		r.glyphs[key] = entry;
		return entry;
	}

	CGContextSetShouldAntialias(context, true);
	CGContextSetShouldSmoothFonts(context, true);
	CGContextSetAllowsFontSmoothing(context, true);
	CGContextSetRGBFillColor(context, 1, 1, 1, 1);
	CGContextSetTextDrawingMode(context, kCGTextFill);
	CGContextSetTextMatrix(context, CGAffineTransformIdentity);
	CGPoint origin = {
		GLYPH_PAD - bounds.origin.x,
		GLYPH_PAD - bounds.origin.y
	};
	CTFontDrawGlyphs(font, &glyph, &origin, 1, context);
	CGContextRelease(context);

	MTLRegion region = MTLRegionMake2D(r.atlasX, r.atlasY, width, height);
	[r.atlas replaceRegion:region mipmapLevel:0 withBytes:pixels
	    bytesPerRow:bytesPerRow];
	free(pixels);

	entry.u0 = (float)r.atlasX / ATLAS_SIZE;
	entry.v0 = (float)r.atlasY / ATLAS_SIZE;
	entry.u1 = (float)(r.atlasX + width) / ATLAS_SIZE;
	entry.v1 = (float)(r.atlasY + height) / ATLAS_SIZE;
	entry.left = bounds.origin.x - GLYPH_PAD;
	entry.top = bounds.origin.y + bounds.size.height + GLYPH_PAD;
	entry.width = width;
	entry.height = height;
	entry.inkWidth = bounds.size.width;
	entry.inkHeight = bounds.size.height;
	entry.colorGlyph = (CTFontGetSymbolicTraits(font) &
	    kCTFontColorGlyphsTrait) != 0;

	r.atlasX += width + 1;
	r.atlasRowHeight = MAX(r.atlasRowHeight, height);
	r.glyphs[key] = entry;
	return entry;
}

static void
drawGlyph(enum MacRenderLayer layer, CTFontRef font, CGGlyph glyph,
		double xPixels, double topPixels, double baselinePixels,
		double maxWidthPixels, double maxHeightPixels, MacColor color)
{
	STGlyph *entry = glyphEntry(font, glyph);
	if (!entry || entry.width <= 0 || entry.height <= 0)
		return;
	double x = xPixels + entry.left;
	double y = baselinePixels - entry.top;
	double width = entry.width;
	double height = entry.height;
	if (entry.colorGlyph && maxWidthPixels > 0 && maxHeightPixels > 0) {
		MacGlyphRect rect = macos_color_glyph_rect(xPixels, topPixels,
		    maxWidthPixels, maxHeightPixels, width, height,
		    entry.inkWidth, entry.inkHeight);
		x = rect.x;
		y = rect.y;
		width = rect.width;
		height = rect.height;
	}
	quad(layer, x, y, width, height, entry.u0, entry.v0,
	    entry.u1, entry.v1, color, entry.colorGlyph ? 2.0f : 1.0f);
}

int
mac_renderer_init(void *viewPtr, const char *fontName, double fontSize)
{
	r.view = (__bridge MTKView *)viewPtr;
	r.device = r.view.device ?: MTLCreateSystemDefaultDevice();
	if (!r.device)
		return 0;
	r.view.device = r.device;
	r.queue = [r.device newCommandQueue];
	r.glyphs = [NSMutableDictionary dictionary];
	r.scale = 1.0;
	r.pointSize = fontSize > 0 ? fontSize : 14.0;
	strlcpy(r.family, fontName ?: "Menlo", sizeof(r.family));

	NSError *error = nil;
	id<MTLLibrary> library = [r.device newLibraryWithSource:shaderSource
	    options:nil error:&error];
	if (!library) {
		fprintf(stderr, "st: Metal shader compile failed: %s\n",
		    error.localizedDescription.UTF8String);
		return 0;
	}
	MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
	pipeline.vertexFunction = [library newFunctionWithName:@"st_vertex"];
	pipeline.fragmentFunction = [library newFunctionWithName:@"st_fragment"];
	pipeline.colorAttachments[0].pixelFormat = r.view.colorPixelFormat;
	pipeline.colorAttachments[0].blendingEnabled = YES;
	pipeline.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
	pipeline.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
	pipeline.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	pipeline.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	pipeline.colorAttachments[0].destinationRGBBlendFactor =
	    MTLBlendFactorOneMinusSourceAlpha;
	pipeline.colorAttachments[0].destinationAlphaBlendFactor =
	    MTLBlendFactorOneMinusSourceAlpha;
	r.pipeline = [r.device newRenderPipelineStateWithDescriptor:pipeline
	    error:&error];
	if (!r.pipeline) {
		fprintf(stderr, "st: Metal pipeline creation failed: %s\n",
		    error.localizedDescription.UTF8String);
		return 0;
	}

	MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
	sampler.minFilter = MTLSamplerMinMagFilterLinear;
	sampler.magFilter = MTLSamplerMinMagFilterLinear;
	sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
	r.sampler = [r.device newSamplerStateWithDescriptor:sampler];
	createAtlas();
	loadFonts();
	return 1;
}

void
mac_renderer_destroy(void)
{
	releaseFonts();
	for (int i = 0; i < MAC_LAYER_COUNT; i++) {
		free(r.layers[i].items);
		r.layers[i].items = NULL;
		r.layers[i].count = r.layers[i].capacity = 0;
	}
	r.glyphs = nil;
	r.atlas = nil;
	r.pipeline = nil;
	r.queue = nil;
}

void
mac_renderer_set_font(const char *fontName, double fontSize)
{
	strlcpy(r.family, fontName ?: "Menlo", sizeof(r.family));
	r.pointSize = MAX(1.0, fontSize);
	createAtlas();
	loadFonts();
}

void
mac_renderer_set_scale(double scale)
{
	scale = MAX(1.0, scale);
	if (fabs(scale - r.scale) < 0.01)
		return;
	r.scale = scale;
	createAtlas();
	loadFonts();
}

double mac_renderer_scale(void) { return r.scale; }
double mac_renderer_font_size(void) { return r.pointSize; }
double mac_renderer_ascent(void) { return r.ascent; }
double mac_renderer_descent(void) { return r.descent; }
double mac_renderer_leading(void) { return r.leading; }
double mac_renderer_cell_width(void) { return r.cellWidth; }
double mac_renderer_cell_height(void) { return r.cellHeight; }

double
mac_renderer_text_width(const char *text, size_t len, double fontScale)
{
	if (!text || !len || !r.fonts[0])
		return 0;
	NSString *string = [[NSString alloc] initWithBytes:text length:len
	    encoding:NSUTF8StringEncoding];
	if (!string)
		return 0;
	CTFontRef font = CTFontCreateCopyWithAttributes(r.fonts[0],
	    CTFontGetSize(r.fonts[0]) * MAX(0.1, fontScale), NULL, NULL);
	CTLineRef line = lineForString(string, font);
	double width = CTLineGetTypographicBounds(line, NULL, NULL, NULL) / r.scale;
	CFRelease(line);
	CFRelease(font);
	return width;
}

int
mac_renderer_begin(MacColor clearColor)
{
	for (int i = 0; i < MAC_LAYER_COUNT; i++)
		listreset(&r.layers[i]);
	r.pass = r.view.currentRenderPassDescriptor;
	r.drawable = r.view.currentDrawable;
	if (!r.pass || !r.drawable)
		return 0;
	r.pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	r.pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	r.pass.colorAttachments[0].clearColor = MTLClearColorMake(clearColor.r,
	    clearColor.g, clearColor.b, clearColor.a);
	r.command = [r.queue commandBuffer];
	r.command.label = @"st Metal frame";
	return r.command != nil;
}

void
mac_renderer_rect(enum MacRenderLayer layer, double x, double y,
		double width, double height, MacColor color)
{
	if (width <= 0 || height <= 0 || layer >= MAC_LAYER_COUNT)
		return;
	float scale = r.scale;
	quad(layer, x * scale, y * scale, width * scale, height * scale,
	    0, 0, 0, 0, color, 0);
}

void
mac_renderer_rune(enum MacRenderLayer layer, uint32_t rune,
		unsigned int style, double x, double top, double baseline,
		double maxWidth, double maxHeight, MacColor color)
{
	if (!rune || !r.fonts[0])
		return;
	uint8_t bytes[8] = {0};
	int len;
	if (rune < 0x80) {
		bytes[0] = rune; len = 1;
	} else if (rune < 0x800) {
		bytes[0] = 0xc0 | (rune >> 6);
		bytes[1] = 0x80 | (rune & 0x3f); len = 2;
	} else if (rune < 0x10000) {
		bytes[0] = 0xe0 | (rune >> 12);
		bytes[1] = 0x80 | ((rune >> 6) & 0x3f);
		bytes[2] = 0x80 | (rune & 0x3f); len = 3;
	} else {
		bytes[0] = 0xf0 | (rune >> 18);
		bytes[1] = 0x80 | ((rune >> 12) & 0x3f);
		bytes[2] = 0x80 | ((rune >> 6) & 0x3f);
		bytes[3] = 0x80 | (rune & 0x3f); len = 4;
	}
	if (style & MAC_FONT_EMOJI) {
		bytes[len++] = 0xef;
		bytes[len++] = 0xb8;
		bytes[len++] = 0x8f;
	}
	NSString *string = [[NSString alloc] initWithBytes:bytes length:len
	    encoding:NSUTF8StringEncoding];
	if (!string)
		return;
	CTLineRef line = lineForString(string, styledFont(style));
	CFArrayRef runs = CTLineGetGlyphRuns(line);
	CFIndex runCount = CFArrayGetCount(runs);
	for (CFIndex i = 0; i < runCount; i++) {
		CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
		NSDictionary *attrs = (__bridge NSDictionary *)CTRunGetAttributes(run);
		CTFontRef font = (__bridge CTFontRef)attrs[(__bridge NSString *)kCTFontAttributeName];
		CFIndex count = CTRunGetGlyphCount(run);
		CGGlyph *glyphs = calloc((size_t)count, sizeof(*glyphs));
		CGPoint *positions = calloc((size_t)count, sizeof(*positions));
		CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs);
		CTRunGetPositions(run, CFRangeMake(0, count), positions);
		for (CFIndex j = 0; j < count; j++)
			drawGlyph(layer, font, glyphs[j],
			    x * r.scale + positions[j].x, top * r.scale,
			    baseline * r.scale - positions[j].y,
			    maxWidth * r.scale, maxHeight * r.scale, color);
		free(glyphs);
		free(positions);
	}
	CFRelease(line);
}

void
mac_renderer_text(enum MacRenderLayer layer, const char *text, size_t len,
		double x, double baseline, double fontScale, MacColor color)
{
	if (!text || !len || !r.fonts[0])
		return;
	NSString *string = [[NSString alloc] initWithBytes:text length:len
	    encoding:NSUTF8StringEncoding];
	if (!string)
		return;
	CTFontRef base = CTFontCreateCopyWithAttributes(r.fonts[0],
	    CTFontGetSize(r.fonts[0]) * MAX(0.1, fontScale), NULL, NULL);
	CTLineRef line = lineForString(string, base);
	CFArrayRef runs = CTLineGetGlyphRuns(line);
	CFIndex runCount = CFArrayGetCount(runs);
	for (CFIndex i = 0; i < runCount; i++) {
		CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, i);
		NSDictionary *attrs = (__bridge NSDictionary *)CTRunGetAttributes(run);
		CTFontRef font = (__bridge CTFontRef)attrs[(__bridge NSString *)kCTFontAttributeName];
		CFIndex count = CTRunGetGlyphCount(run);
		CGGlyph *glyphs = calloc((size_t)count, sizeof(*glyphs));
		CGPoint *positions = calloc((size_t)count, sizeof(*positions));
		CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs);
		CTRunGetPositions(run, CFRangeMake(0, count), positions);
		for (CFIndex j = 0; j < count; j++)
			drawGlyph(layer, font, glyphs[j], x * r.scale + positions[j].x,
			    0, baseline * r.scale - positions[j].y, 0, 0, color);
		free(glyphs);
		free(positions);
	}
	CFRelease(line);
	CFRelease(base);
}

void
mac_renderer_end(void)
{
	if (!r.command || !r.pass || !r.drawable)
		return;
	id<MTLRenderCommandEncoder> encoder =
	    [r.command renderCommandEncoderWithDescriptor:r.pass];
	[encoder setRenderPipelineState:r.pipeline];
	vector_float2 viewport = {(float)r.view.drawableSize.width,
	    (float)r.view.drawableSize.height};
	[encoder setVertexBytes:&viewport length:sizeof(viewport) atIndex:1];
	[encoder setFragmentTexture:r.atlas atIndex:0];
	[encoder setFragmentSamplerState:r.sampler atIndex:0];
	for (int i = 0; i < MAC_LAYER_COUNT; i++) {
		MacVertexList *list = &r.layers[i];
		if (!list->count)
			continue;
		id<MTLBuffer> buffer = [r.device newBufferWithBytes:list->items
		    length:list->count * sizeof(*list->items)
		    options:MTLResourceStorageModeShared];
		[encoder setVertexBuffer:buffer offset:0 atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
		    vertexCount:list->count];
	}
	[encoder endEncoding];
	[r.command presentDrawable:r.drawable];
	[r.command commit];
	r.command = nil;
	r.pass = nil;
	r.drawable = nil;
}

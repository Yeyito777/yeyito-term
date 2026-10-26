/* st video on macOS: APC commands through graphics.c into the VideoToolbox
 * and AVFoundation player, frames composited by the real Metal renderer into
 * an offscreen texture.  The media is encoded here with VideoToolbox and
 * AudioConverter; sound plays at volume 0.  No window is opened. */
#define main st_backend_main
#include "../macos/backend.m"
#undef main
#include "../macos/renderer.m"
#import <AudioToolbox/AudioToolbox.h>
#import <VideoToolbox/VideoToolbox.h>
#include <poll.h>
#include <termios.h>
#include <util.h>
#include "test.h"

#define FPS 30
#define FRAME_US (1000000 / FPS)
#define VIDEO_FRAMES 30
#define RATE 48000
#define AUDIO_FRAMES 48	/* 48 x 1024 samples: 1.02 s */

typedef struct {
	unsigned char *data;
	size_t length;
	int64_t pts;
	int keyframe;
} Encoded;

static Encoded video[VIDEO_FRAMES];
static int videoCount;
static Encoded audio[AUDIO_FRAMES + 8];
static int audioCount;

static id<MTLTexture> target;
static unsigned char pixels[128 * 128 * 4];
static int redraws;
static GraphicsPlacementView placed;
static int placedCount;
static char response[512];
static int opened;

/* ---- media ------------------------------------------------------------ */

static void
append(Encoded *frame, const void *bytes, size_t length)
{
	frame->data = realloc(frame->data, frame->length + length);
	memcpy(frame->data + frame->length, bytes, length);
	frame->length += length;
}

/* VideoToolbox emits length-prefixed NALs with parameter sets in the format
 * description; the wire wants Annex B with SPS/PPS on keyframes. */
static void
encodedVideo(void *refcon, void *frameRefcon, OSStatus status,
		VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
	static const unsigned char startCode[4] = {0, 0, 0, 1};
	(void)refcon; (void)frameRefcon; (void)flags;
	if (status != noErr || !sample || videoCount == VIDEO_FRAMES)
		return;
	Encoded *frame = &video[videoCount++];
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
	CFDictionaryRef first = attachments && CFArrayGetCount(attachments) ?
	    CFArrayGetValueAtIndex(attachments, 0) : NULL;
	frame->keyframe = !first ||
	    !CFDictionaryContainsKey(first, kCMSampleAttachmentKey_NotSync);
	frame->pts = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample),
	    1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
	if (frame->keyframe) {
		CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
		size_t count = 0;
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL,
		    NULL, &count, NULL);
		for (size_t i = 0; i < count; i++) {
			const uint8_t *set;
			size_t size;
			CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i,
			    &set, &size, NULL, NULL);
			append(frame, startCode, 4);
			append(frame, set, size);
		}
	}
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
	size_t total = CMBlockBufferGetDataLength(block), offset = 0;
	unsigned char *bytes = malloc(total);
	CMBlockBufferCopyDataBytes(block, 0, total, bytes);
	while (offset + 4 <= total) {
		size_t length = (size_t)bytes[offset] << 24 | (size_t)bytes[offset + 1] << 16 |
		    (size_t)bytes[offset + 2] << 8 | bytes[offset + 3];
		offset += 4;
		if (length > total - offset)
			break;
		append(frame, startCode, 4);
		append(frame, bytes + offset, length);
		offset += length;
	}
	free(bytes);
}

/* Frames 0..14 red, 15..29 blue: which one is on screen shows the clock. */
static int
makeVideo(void)
{
	VTCompressionSessionRef session = NULL;
	if (VTCompressionSessionCreate(NULL, 160, 96, kCMVideoCodecType_H264,
	    NULL, NULL, NULL, encodedVideo, NULL, &session) != noErr)
		return 0;
	VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
	    kCFBooleanFalse);
	VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
	    (__bridge CFNumberRef)@15);
	VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime,
	    kCFBooleanFalse);
	for (int i = 0; i < VIDEO_FRAMES; i++) {
		CVPixelBufferRef buffer = NULL;
		CVPixelBufferCreate(NULL, 160, 96, kCVPixelFormatType_32BGRA,
		    (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}},
		    &buffer);
		CVPixelBufferLockBaseAddress(buffer, 0);
		unsigned char *base = CVPixelBufferGetBaseAddress(buffer);
		size_t stride = CVPixelBufferGetBytesPerRow(buffer);
		for (int y = 0; y < 96; y++)
			for (int x = 0; x < 160; x++) {
				unsigned char *p = base + y * stride + x * 4;
				p[0] = i < 15 ? 0 : 255;	/* B */
				p[1] = 0;
				p[2] = i < 15 ? 255 : 0;	/* R */
				p[3] = 255;
			}
		CVPixelBufferUnlockBaseAddress(buffer, 0);
		VTCompressionSessionEncodeFrame(session, buffer,
		    CMTimeMake(i * FRAME_US, 1000000), CMTimeMake(FRAME_US, 1000000),
		    NULL, NULL, NULL);
		CVPixelBufferRelease(buffer);
	}
	VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
	VTCompressionSessionInvalidate(session);
	CFRelease(session);
	return videoCount == VIDEO_FRAMES && video[0].keyframe;
}

typedef struct {
	float *samples;
	UInt32 frames, used;
} PCMSource;

static OSStatus
pcmInput(AudioConverterRef converter, UInt32 *packets, AudioBufferList *data,
		AudioStreamPacketDescription **descriptions, void *context)
{
	PCMSource *source = context;
	UInt32 count = MIN(*packets, source->frames - source->used);
	(void)converter; (void)descriptions;
	*packets = count;
	data->mBuffers[0].mData = source->samples + 2 * source->used;
	data->mBuffers[0].mDataByteSize = count * 8;
	data->mBuffers[0].mNumberChannels = 2;
	source->used += count;
	return noErr;
}

static int
makeAudio(void)
{
	AudioStreamBasicDescription pcm = {
		.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 8, .mFramesPerPacket = 1, .mBytesPerFrame = 8,
		.mChannelsPerFrame = 2, .mBitsPerChannel = 32,
	};
	AudioStreamBasicDescription aac = {
		.mSampleRate = RATE, .mFormatID = kAudioFormatMPEG4AAC,
		.mFormatFlags = kMPEG4Object_AAC_LC, .mFramesPerPacket = 1024,
		.mChannelsPerFrame = 2,
	};
	AudioConverterRef converter = NULL;
	PCMSource source = {.frames = (AUDIO_FRAMES + 4) * 1024};
	unsigned char buffer[4096];

	if (AudioConverterNew(&pcm, &aac, &converter) != noErr)
		return 0;
	source.samples = calloc(source.frames * 2, sizeof(float));
	for (UInt32 i = 0; i < source.frames; i++)
		source.samples[2 * i] = source.samples[2 * i + 1] =
		    0.2f * sinf(2 * (float)M_PI * 440 * i / RATE);
	while (audioCount < AUDIO_FRAMES) {
		AudioBufferList list = {1, {{2, sizeof(buffer), buffer}}};
		AudioStreamPacketDescription description;
		UInt32 packets = 1;
		if (AudioConverterFillComplexBuffer(converter, pcmInput, &source,
		    &packets, &list, &description) != noErr || !packets)
			break;
		Encoded *frame = &audio[audioCount];
		append(frame, buffer + description.mStartOffset,
		    description.mDataByteSize);
		frame->pts = (int64_t)audioCount * 1024 * 1000000 / RATE;
		audioCount++;
	}
	AudioConverterDispose(converter);
	free(source.samples);
	return audioCount == AUDIO_FRAMES;
}

/* ---- terminal side ---------------------------------------------------- */

static void
base64(const unsigned char *source, size_t length, char *output)
{
	static const char digits[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, used = 0;
	for (i = 0; i + 2 < length; i += 3) {
		output[used++] = digits[source[i] >> 2];
		output[used++] = digits[((source[i] & 3) << 4) | (source[i+1] >> 4)];
		output[used++] = digits[((source[i+1] & 15) << 2) | (source[i+2] >> 6)];
		output[used++] = digits[source[i+2] & 63];
	}
	if (length - i == 1) {
		output[used++] = digits[source[i] >> 2];
		output[used++] = digits[(source[i] & 3) << 4];
		output[used++] = '='; output[used++] = '=';
	} else if (length - i == 2) {
		output[used++] = digits[source[i] >> 2];
		output[used++] = digits[((source[i] & 3) << 4) | (source[i+1] >> 4)];
		output[used++] = digits[(source[i+1] & 15) << 2];
		output[used++] = '=';
	}
	output[used] = '\0';
}

static void
apc(const char *controls, const void *payload, size_t length)
{
	size_t size = strlen(controls) + 4 + (length + 2) / 3 * 4;
	char *sequence = malloc(size);
	GraphicsCommandResult result;
	int prefix = snprintf(sequence, size, "G%s%s", controls, length ? ";" : "");
	if (length)
		base64(payload, length, sequence + prefix);
	graphics_handle_apc(sequence, strlen(sequence), tlineviewline(1), 1, 0,
	    win.cw, win.ch, trow(), tlineviewrow, 1, &result);
	snprintf(response, sizeof(response), "%.*s", (int)result.response_len,
	    result.response);
	free(sequence);
}

static void
text(const char *controls, const char *payload)
{
	apc(controls, payload, strlen(payload));
}

static void
sendPacket(int stream, const Encoded *frame, int64_t offset)
{
	unsigned char *packet = malloc(VIDEO_PACKET_HEADER + frame->length);
	uint64_t pts = (uint64_t)(frame->pts + offset);
	packet[0] = (unsigned char)stream;
	packet[1] = (unsigned char)frame->keyframe;
	for (int i = 0; i < 8; i++)
		packet[2 + i] = (unsigned char)(pts >> (56 - 8 * i));
	memcpy(packet + VIDEO_PACKET_HEADER, frame->data, frame->length);
	apc("a=v,i=77,k=d", packet, VIDEO_PACKET_HEADER + frame->length);
	free(packet);
}

static void
sendMedia(int withAudio, int64_t offset)
{
	int a = 0;
	for (int v = 0; v < VIDEO_FRAMES; v++) {
		while (withAudio && a < audioCount && audio[a].pts <= video[v].pts)
			sendPacket(VIDEO_STREAM_AUDIO, &audio[a++], offset);
		sendPacket(VIDEO_STREAM_VIDEO, &video[v], offset);
	}
	while (withAudio && a < audioCount)
		sendPacket(VIDEO_STREAM_AUDIO, &audio[a++], offset);
}

static void
countRedraw(void)
{
	redraws++;
}

static void
capturePlacement(const GraphicsPlacementView *view, void *context)
{
	(void)context;
	placed = *view;
	placedCount++;
}

/* Spin the main queue (VideoToolbox's callbacks land there) and tick. */
static void
spin(double seconds)
{
	double end = CACurrentMediaTime() + seconds;
	do {
		[NSRunLoop.mainRunLoop runUntilDate:[NSDate
		    dateWithTimeIntervalSinceNow:0.004]];
		mac_video_tick(CACurrentMediaTime() + 1.0 / 120);
	} while (CACurrentMediaTime() < end);
}

static long long
statusField(const char *name)
{
	char key[32];
	const char *p;
	text("a=v,i=77,k=q", "seq=1");
	snprintf(key, sizeof(key), " %s=", name);
	p = strstr(response, key);
	return p ? strtoll(p + strlen(key), NULL, 10) : LLONG_MIN;
}

static int
stateIs(const char *state)
{
	char key[32];
	statusField("pos");
	snprintf(key, sizeof(key), " state=%s ", state);
	return strstr(response, key) != NULL;
}

/* Spin until the state is reached; fails fast instead of hanging. */
static int
waitState(const char *state, double timeout)
{
	double end = CACurrentMediaTime() + timeout;
	while (CACurrentMediaTime() < end) {
		if (stateIs(state))
			return 1;
		spin(0.01);
	}
	return stateIs(state);
}

/* Render the placement through drawGraphicsPlacement and read the pixel at
 * its centre: 'r', 'b', '-' (nothing drawn) or '?'. */
static char
shownColor(void)
{
	placedCount = 0;
	tlineviewprepare();
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, win.cw, win.ch, trow(),
	    tlineviewrow, capturePlacement, NULL);
	if (placedCount != 1)
		return '!';
	for (int i = 0; i < MAC_LAYER_COUNT; i++)
		listreset(&r.layers[i]);
	for (int i = 0; i < 3; i++)
		r.imageLayers[i].count = 0;
	image_occlusion_begin(&imageOcclusion);
	mac_renderer_set_image_clip(borderpx, borderpx, win.tw, win.th);
	drawGraphicsPlacement(&placed, (void *)(intptr_t)GRAPHICS_STAGE_ABOVE_TEXT);
	image_occlusion_end(&imageOcclusion);
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	id<MTLCommandBuffer> command = [r.queue commandBuffer];
	id<MTLRenderCommandEncoder> encoder =
	    [command renderCommandEncoderWithDescriptor:pass];
	encodeFrame(encoder);
	[encoder endEncoding];
	[command commit];
	[command waitUntilCompleted];
	[target getBytes:pixels bytesPerRow:128 * 4
	    fromRegion:MTLRegionMake2D(0, 0, 128, 128) mipmapLevel:0];
	int px = (int)cellX(placed.column) + 2 * win.cw;
	int py = (int)cellY(placed.row) + win.ch;
	unsigned char *p = &pixels[(py * 128 + px) * 4];	/* BGRA */
	if (p[2] > 180 && p[0] < 80 && p[1] < 80)
		return 'r';
	if (p[0] > 180 && p[2] < 80 && p[1] < 80)
		return 'b';
	if (p[0] < 20 && p[1] < 20 && p[2] < 20)
		return '-';
	return '?';
}

static void
openVideo(int withAudio, int prebuffer)
{
	char config[256];
	graphics_reset();
	snprintf(config, sizeof(config), "video=h264 width=160 height=96 "
	    "prebuffer=%d volume=0%s", prebuffer,
	    withAudio ? " audio=aac rate=48000 channels=2" : "");
	text("a=v,i=77,k=o", config);
	opened = strcmp(response, "\033_Gi=77;OK\033\\") == 0;
	apc("a=p,i=77,p=1,c=4,r=2,C=1,q=2", NULL, 0);
}

TEST(capabilities)
{
	apc("a=v,i=5,k=c", NULL, 0);
	ASSERT_STR_EQ("\033_Gi=5;OK video=h264 audio=aac max=4096x4096\033\\",
	    response);
}

TEST(video_plays_on_the_host_clock_and_ends)
{
	openVideo(0, 100);
	ASSERT(opened);
	ASSERT_EQ('-', shownColor());	/* nothing decoded yet */
	ASSERT(stateIs("buffering"));
	ASSERT(statusField("pos") == -1);
	sendMedia(0, 0);
	text("a=v,i=77,k=e,q=1", "");
	ASSERT(waitState("playing", 2));
	spin(0.15);
	ASSERT_EQ('r', shownColor());
	long long pos = statusField("pos");
	ASSERT(pos > 50000 && pos < 450000);
	ASSERT(redraws > 0);
	spin(0.45);
	ASSERT_EQ('b', shownColor());
	ASSERT(waitState("ended", 2));
	ASSERT(statusField("shown") >= 20);
	ASSERT(statusField("vbuf") == (VIDEO_FRAMES - 1) * FRAME_US);
	ASSERT(statusField("rx") > 1000);
	ASSERT(strstr(response, "error=") == NULL);
	/* The last frame stays. */
	ASSERT_EQ('b', shownColor());
	ASSERT(!mac_video_active());
}

TEST(audio_clock_pause_and_resume)
{
	openVideo(1, 200);
	sendMedia(1, 0);
	ASSERT(waitState("playing", 2));
	ASSERT(statusField("abuf") == (AUDIO_FRAMES - 1) * 1024LL * 1000000 / RATE);
	spin(0.1);
	text("a=v,i=77,k=s,q=1", "pause=1");
	ASSERT(stateIs("paused"));
	long long held = statusField("pos");
	char color = shownColor();
	spin(0.3);
	ASSERT(statusField("pos") == held);
	ASSERT_EQ(color, shownColor());
	ASSERT(!mac_video_active());
	text("a=v,i=77,k=s,q=1", "pause=0");
	ASSERT(waitState("playing", 2));
	spin(0.2);
	ASSERT(statusField("pos") > held + 100000);
	/* Without end of stream it runs dry and waits for more. */
	ASSERT(waitState("buffering", 2));
	ASSERT(strstr(response, "error=") == NULL);
}

TEST(flush_seeks_exactly)
{
	openVideo(1, 100);
	sendMedia(1, 0);
	ASSERT(waitState("playing", 2));
	/* Seek to 0.6 s: the keyframe at 0.5 s decodes, its frame is skipped. */
	text("a=v,i=77,k=f,q=1", "start=600000");
	ASSERT(stateIs("buffering"));
	ASSERT(statusField("vbuf") == -1);
	long long dropped = statusField("dropped");
	for (int v = 15; v < VIDEO_FRAMES; v++)
		sendPacket(VIDEO_STREAM_VIDEO, &video[v], 0);
	for (int a = 0; a < audioCount; a++)
		if (audio[a].pts >= 500000)
			sendPacket(VIDEO_STREAM_AUDIO, &audio[a], 0);
	text("a=v,i=77,k=e,q=1", "");
	ASSERT(waitState("playing", 2));
	ASSERT(statusField("pos") >= 600000);
	ASSERT(statusField("dropped") >= dropped + 3);
	spin(0.02);
	ASSERT_EQ('b', shownColor());
	ASSERT(waitState("ended", 2));
}

/* ---- through the terminal parser ------------------------------------ */

/* Feed bytes to st's real input path: a PTY whose slave is st's tty line,
 * read with ttyread() in whatever pieces the PTY hands over. */
static int ptyMaster = -1, ptySlave = -1;

static int
openTerminalLine(void)
{
	char name[128];
	struct termios mode;

	if (openpty(&ptyMaster, &ptySlave, name, NULL, NULL) < 0)
		return 0;
	tcgetattr(ptySlave, &mode);
	cfmakeraw(&mode);
	tcsetattr(ptySlave, TCSANOW, &mode);
	fcntl(ptyMaster, F_SETFL, fcntl(ptyMaster, F_GETFL) | O_NONBLOCK);
	int input = dup(0);
	ttynew(name, NULL, NULL, NULL);	/* opens the line as st's tty */
	dup2(input, 0);
	close(input);
	return 1;
}

static void
drainTerminalLine(void)
{
	struct pollfd readable = {ptySlave, POLLIN, 0};
	while (poll(&readable, 1, 0) > 0 && (readable.revents & POLLIN))
		ttyread();
}

static int
writeTerminalLine(const char *data, size_t length)
{
	size_t offset = 0;
	double end = CACurrentMediaTime() + 10;

	while (offset < length && CACurrentMediaTime() < end) {
		ssize_t n = write(ptyMaster, data + offset, length - offset);
		if (n > 0)
			offset += (size_t)n;
		drainTerminalLine();
	}
	drainTerminalLine();
	return offset == length;
}

typedef struct {
	char *data;
	size_t length, capacity;
} Stream;

static void
streamAppend(Stream *stream, const char *bytes, size_t length)
{
	if (stream->length + length > stream->capacity) {
		stream->capacity = (stream->length + length) * 2;
		stream->data = realloc(stream->data, stream->capacity);
	}
	memcpy(stream->data + stream->length, bytes, length);
	stream->length += length;
}

/* An APC as an application writes it: payloads over 4096 base64 bytes are
 * split into m=1 chunks. */
static void
streamAPC(Stream *stream, const char *controls, const void *payload,
		size_t length)
{
	size_t encoded = (length + 2) / 3 * 4, offset = 0;
	char *base = malloc(encoded + 1);
	char head[128];

	base64(payload, length, base);
	do {
		size_t chunk = MIN(encoded - offset, (size_t)4096);
		int more = offset + chunk < encoded;
		if (!offset)
			snprintf(head, sizeof(head), "\033_G%s%s%s", controls,
			    more ? ",m=1" : "", length ? ";" : "");
		else
			snprintf(head, sizeof(head), "\033_Gm=%d;", more);
		streamAppend(stream, head, strlen(head));
		streamAppend(stream, base + offset, chunk);
		streamAppend(stream, "\033\\", 2);
		offset += chunk;
	} while (offset < encoded);
	free(base);
}

static void
streamPacket(Stream *stream, int stream_id, const Encoded *frame)
{
	unsigned char *packet = malloc(VIDEO_PACKET_HEADER + frame->length);
	packet[0] = (unsigned char)stream_id;
	packet[1] = (unsigned char)frame->keyframe;
	for (int i = 0; i < 8; i++)
		packet[2 + i] = (unsigned char)((uint64_t)frame->pts >> (56 - 8 * i));
	memcpy(packet + VIDEO_PACKET_HEADER, frame->data, frame->length);
	streamAPC(stream, "a=v,i=77,k=d", packet, VIDEO_PACKET_HEADER + frame->length);
	free(packet);
}

TEST(stream_through_the_terminal_parser)
{
	static const char config[] = "video=h264 width=160 height=96 prebuffer=100 "
	    "volume=0 audio=aac rate=48000 channels=2";
	Stream stream = {0};
	int a = 0;
	size_t offset = 0;
	unsigned seed = 12345;

	graphics_reset();
	/* Text, then the video placed where the cursor is (row 1, column 1). */
	streamAppend(&stream, "\033[H\r\nx", 6);
	streamAPC(&stream, "a=v,i=77,k=o,q=2", config, strlen(config));
	streamAPC(&stream, "a=p,i=77,p=1,c=4,r=2,C=1,q=2", NULL, 0);
	for (int v = 0; v < VIDEO_FRAMES; v++) {
		while (a < audioCount && audio[a].pts <= video[v].pts)
			streamPacket(&stream, VIDEO_STREAM_AUDIO, &audio[a++]);
		streamPacket(&stream, VIDEO_STREAM_VIDEO, &video[v]);
	}
	while (a < audioCount)
		streamPacket(&stream, VIDEO_STREAM_AUDIO, &audio[a++]);
	streamAPC(&stream, "a=v,i=77,k=e,q=2", NULL, 0);
	/* Odd-sized writes split escapes, base64 and chunk boundaries. */
	ASSERT(ptyMaster >= 0 || openTerminalLine());
	while (offset < stream.length) {
		seed = seed * 1103515245 + 12345;
		size_t piece = MIN(stream.length - offset, (size_t)(1 + seed % 997));
		ASSERT(writeTerminalLine(stream.data + offset, piece));
		offset += piece;
	}
	free(stream.data);
	ASSERT_EQ(1, (int)graphics_video_count());
	ASSERT(statusField("rx") > 1000);
	ASSERT(strstr(response, "error=") == NULL);
	ASSERT(statusField("vbuf") == (VIDEO_FRAMES - 1) * FRAME_US);
	ASSERT(waitState("playing", 2));
	spin(0.6);
	ASSERT_EQ('b', shownColor());
	ASSERT_EQ(1, placed.row);
	ASSERT_EQ(1, placed.column);
	ASSERT(waitState("ended", 2));
	ASSERT(statusField("shown") >= 20);
}

TEST(errors_are_reported_and_delete_stops)
{
	unsigned char garbage[64];
	openVideo(0, 100);
	memset(garbage, 0x41, sizeof(garbage));
	Encoded bad = {garbage, sizeof(garbage), 0, 1};
	sendPacket(VIDEO_STREAM_AUDIO, &bad, 0);	/* no audio stream */
	ASSERT(statusField("pos") == -1);
	ASSERT(strstr(response, " error=EINVAL") != NULL);
	sendMedia(0, 0);
	ASSERT(waitState("playing", 2));
	apc("a=d,d=I,i=77,q=2", NULL, 0);
	ASSERT_EQ(0, (int)graphics_video_count());
	text("a=v,i=77,k=q", "");
	ASSERT(strstr(response, "ENOENT") != NULL);
	spin(0.05);
	ASSERT(!mac_video_active());
}

int
main(void)
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device) {
			puts("SKIP: video tests require a Metal device");
			return 0;
		}
		MTKView *view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128)
		    device:device];
		view.paused = YES;
		view.autoResizeDrawable = NO;
		view.drawableSize = CGSizeMake(128, 128);
		if (!mac_renderer_init((__bridge void *)view, "Menlo", 12))
			return 1;
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		    width:128 height:128 mipmapped:NO];
		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeShared;
		target = [device newTextureWithDescriptor:descriptor];
		xloadcols();
		palette[defaultbg] = (MacColor){0, 0, 0, 1};
		palette[defaultfg] = (MacColor){1, 1, 1, 1};
		tnew(8, 4);
		win.cw = win.ch = 16;
		win.tw = 8 * win.cw;
		win.th = 4 * win.ch;
		graphics_set_image_free_callback(freeGraphicsImage, NULL);
		mac_video_init((__bridge void *)device, NULL, countRedraw);
		if (!makeVideo() || !makeAudio()) {
			puts("SKIP: VideoToolbox/AudioConverter encoders unavailable");
			return 0;
		}
		RUN_TEST(capabilities);
		RUN_TEST(video_plays_on_the_host_clock_and_ends);
		RUN_TEST(audio_clock_pause_and_resume);
		RUN_TEST(flush_seeks_exactly);
		RUN_TEST(stream_through_the_terminal_parser);
		RUN_TEST(errors_are_reported_and_delete_stops);
		graphics_reset();
		mac_video_shutdown();
		mac_renderer_destroy();
		return test_summary();
	}
}

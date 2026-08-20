/* Kitty graphics protocol parser, lifecycle, and placement tests. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "test.h"
#include "../graphics.h"

static Glyph line_a[8], line_b[8], line_c[8], line_d[8];
static int freed_images;
static int drawn;
static GraphicsPlacementView last_draw;

typedef struct {
	int x, y, width, height;
} SelectedRegion;

static void
free_image(uint64_t serial, void *context)
{
	(void)serial;
	(void)context;
	freed_images++;
}

static int
line_row(Line line, int alt)
{
	if (alt)
		return INT_MIN;
	if (line == line_a)
		return 4;
	if (line == line_b)
		return -1;
	if (line == line_c)
		return 5;
	if (line == line_d)
		return 6;
	return INT_MIN;
}

static Line
reflow_line(void *context, long long index)
{
	(void)context;
	if (index == 10) return line_c;
	if (index == 11) return line_d;
	return NULL;
}

static void
capture_draw(const GraphicsPlacementView *view, void *context)
{
	(void)context;
	drawn++;
	last_draw = *view;
}

static int
region_selected(int x, int y, int width, int height, void *context)
{
	SelectedRegion *region = context;
	return x < region->x + region->width && x + width > region->x &&
	    y < region->y + region->height && y + height > region->y;
}

static uint32_t
png_u32(const unsigned char *data)
{
	return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
	    (uint32_t)data[2] << 8 | data[3];
}

static GraphicsCommandResult
command(const char *sequence, Line anchor)
{
	GraphicsCommandResult result;
	graphics_handle_apc(sequence, strlen(sequence), anchor, 2, 0, 10, 20,
	    24, line_row, 1, &result);
	return result;
}

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
reset_state(void)
{
	graphics_reset();
	graphics_set_image_free_callback(free_image, NULL);
	freed_images = 0;
	drawn = 0;
	memset(&last_draw, 0, sizeof(last_draw));
}

TEST(query_direct_rgb)
{
	GraphicsCommandResult result;
	reset_state();
	result = command("Ga=q,f=24,s=1,v=1,i=31;AAAA", line_a);
	ASSERT(result.handled);
	ASSERT(!result.redraw);
	ASSERT(strstr(result.response, "i=31;OK") != NULL);
	ASSERT_EQ(0, (int)graphics_image_count());
}

TEST(chunked_rgba_placement)
{
	GraphicsCommandResult first, second;
	reset_state();
	first = command("Ga=T,f=32,s=1,v=1,i=42,p=7,c=2,r=3,C=1,m=1;/wAA",
	    line_a);
	ASSERT(first.handled);
	ASSERT(!first.redraw);
	ASSERT_EQ(0, (int)graphics_image_count());
	second = command("Gm=0;/w==", line_a);
	ASSERT(second.redraw);
	ASSERT(!second.move_cursor);
	ASSERT(strstr(second.response, "i=42,p=7;OK") != NULL);
	ASSERT_EQ(1, (int)graphics_image_count());
	ASSERT_EQ(1, (int)graphics_placement_count());
	ASSERT_EQ(4, (int)graphics_image_bytes());

	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT_EQ(42, (int)last_draw.image_id);
	ASSERT_EQ(7, (int)last_draw.placement_id);
	ASSERT_EQ(2, last_draw.column);
	ASSERT_EQ(4, last_draw.row);
	ASSERT_EQ(2, last_draw.columns);
	ASSERT_EQ(3, last_draw.rows);
	ASSERT_EQ(255, last_draw.rgba[0]);
	ASSERT_EQ(0, last_draw.rgba[1]);
	ASSERT_EQ(0, last_draw.rgba[2]);
	ASSERT_EQ(255, last_draw.rgba[3]);
}

TEST(natural_size_and_cursor_movement)
{
	GraphicsCommandResult result;
	reset_state();
	result = command("Ga=T,f=24,s=1,v=1,i=2;AP8A", line_a);
	ASSERT(result.redraw);
	ASSERT(result.move_cursor);
	ASSERT_EQ(1, result.columns);
	ASSERT_EQ(1, result.rows);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT(last_draw.natural_size);
}

TEST(negative_z_stage_and_reanchor)
{
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=3,z=-1,c=1,r=2,C=1;AAAA", line_a);
	graphics_reanchor_line(line_a, line_b);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(0, drawn);
	graphics_draw(0, GRAPHICS_STAGE_BELOW_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT_EQ(-1, last_draw.row);
}

TEST(fully_offscreen_placement_is_culled)
{
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=4,C=1;AAAA", line_b);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(0, drawn);
}

TEST(recycle_removes_placement_not_addressed_image)
{
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=9,C=1;AAAA", line_a);
	graphics_recycle_line(line_a);
	ASSERT_EQ(0, (int)graphics_placement_count());
	ASSERT_EQ(1, (int)graphics_image_count());
	ASSERT_EQ(0, freed_images);
	command("Ga=d,d=I,i=9", line_a);
	ASSERT_EQ(0, (int)graphics_image_count());
	ASSERT_EQ(1, freed_images);
}

TEST(reflow_updates_anchor_and_column)
{
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=10,C=1;AAAA", line_a);
	ASSERT_EQ(3, graphics_line_extent(line_a));
	graphics_reflow_line(line_a, 39, 40, 10, reflow_line, NULL);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT_EQ(6, last_draw.row);
	ASSERT_EQ(1, last_draw.column);
}

TEST(clear_buffer_keeps_reusable_data)
{
	GraphicsCommandResult result;
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=12,C=1;AAAA", line_a);
	graphics_clear_buffer(0);
	ASSERT_EQ(0, (int)graphics_placement_count());
	ASSERT_EQ(1, (int)graphics_image_count());
	result = command("Ga=p,i=12,p=4,c=1,r=1,C=1", line_b);
	ASSERT(result.redraw);
	ASSERT(strstr(result.response, "i=12,p=4;OK") != NULL);
	ASSERT_EQ(1, (int)graphics_placement_count());
}

TEST(hard_delete_all_reclaims_visible_image_data)
{
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=20,C=1;AAAA", line_a);
	command("Ga=T,f=24,s=1,v=1,i=21,C=1;AAAA", line_b);
	command("Ga=d,d=A", line_a);
	ASSERT_EQ(1, (int)graphics_placement_count());
	ASSERT_EQ(1, (int)graphics_image_count());
	ASSERT_EQ(1, freed_images);
}

TEST(position_and_z_delete_selectors)
{
	reset_state();
	command("Ga=T,f=24,s=1,v=1,i=22,p=1,c=3,r=2,z=5,C=1;AAAA",
	    line_a);
	command("Ga=d,d=p,x=3,y=5", line_a);
	ASSERT_EQ(0, (int)graphics_placement_count());
	command("Ga=p,i=22,p=2,c=3,r=2,z=5,C=1", line_a);
	command("Ga=d,d=z,z=5", line_a);
	ASSERT_EQ(0, (int)graphics_placement_count());
}

TEST(unsupported_transport_and_quiet_errors)
{
	GraphicsCommandResult result;
	reset_state();
	result = command("Ga=q,t=f,f=24,s=1,v=1,i=8;L3RtcC94", line_a);
	ASSERT(strstr(result.response, "ENOSYS") != NULL);
	result = command("Ga=q,t=f,f=24,s=1,v=1,i=8,q=2;L3RtcC94", line_a);
	ASSERT_EQ(0, (int)result.response_len);
	result = command("Ga=q,f=24,s=1,v=1,i=8,q=2;AAAA", line_a);
	ASSERT_EQ(0, (int)result.response_len);
}

TEST(invalid_payload_does_not_allocate)
{
	GraphicsCommandResult result;
	reset_state();
	result = command("Ga=T,f=32,s=100000,v=100000,i=1;AAAA", line_a);
	ASSERT(strstr(result.response, "EINVAL") != NULL);
	ASSERT_EQ(0, (int)graphics_image_count());
	ASSERT_EQ(0, (int)graphics_placement_count());
}

TEST(png_query)
{
	GraphicsCommandResult result;
	reset_state();
	result = command("Ga=q,f=100,i=55;iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAHnOcQAAAAABJRU5ErkJggg==",
	    line_a);
	ASSERT(strstr(result.response, "i=55;OK") != NULL);
	ASSERT_EQ(0, (int)graphics_image_count());
}

TEST(png_pixels_compact_and_restore)
{
	GraphicsCommandResult result;
	const unsigned char *pixels;
	uint64_t serial;
	size_t expanded, compact;

	reset_state();
	result = command("Ga=T,f=100,i=155,C=1;iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAHnOcQAAAAABJRU5ErkJggg==",
	    line_a);
	ASSERT(result.redraw);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT(last_draw.rgba != NULL);
	serial = last_draw.serial;
	expanded = graphics_image_bytes();

	graphics_compact_images();
	compact = graphics_image_bytes();
	ASSERT(compact < expanded);
	pixels = graphics_image_pixels(serial);
	ASSERT(pixels != NULL);
	ASSERT_EQ((int)expanded, (int)graphics_image_bytes());
	graphics_release_image_pixels(serial);
	ASSERT_EQ((int)compact, (int)graphics_image_bytes());
}

TEST(selected_placement_copies_cropped_png_atomically)
{
	GraphicsCommandResult result;
	SelectedRegion selected = {0, 5, 8, 1};
	unsigned char *png = NULL, raw[5];
	size_t png_length = 0, offset;
	uLongf raw_length = sizeof(raw);

	reset_state();
	/* Two RGBA pixels; copy only the second source pixel. The placement spans
	 * rows 4-5, and selecting only row 5 still selects the whole object. */
	result = command("Ga=T,f=32,s=2,v=1,x=1,w=1,c=3,r=2,C=1;AQIDBAUGBwg=",
	    line_a);
	ASSERT(result.redraw);
	ASSERT(graphics_selection_png(0, line_row, region_selected, &selected,
	    &png, &png_length));
	ASSERT(png != NULL);
	ASSERT(png_length > 57);
	ASSERT(memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0);
	ASSERT_EQ(1, (int)png_u32(png + 16));
	ASSERT_EQ(1, (int)png_u32(png + 20));

	/* The encoder emits IHDR followed by one IDAT chunk. */
	offset = 8 + 25;
	ASSERT(memcmp(png + offset + 4, "IDAT", 4) == 0);
	ASSERT_EQ(Z_OK, uncompress(raw, &raw_length, png + offset + 8,
	    png_u32(png + offset)));
	ASSERT_EQ(5, (int)raw_length);
	ASSERT_EQ(0, raw[0]);
	ASSERT_EQ(5, raw[1]);
	ASSERT_EQ(6, raw[2]);
	ASSERT_EQ(7, raw[3]);
	ASSERT_EQ(8, raw[4]);
	free(png);
}

TEST(kitten_icat_unpadded_base64)
{
	GraphicsCommandResult result;
	reset_state();
	/* kitten icat strips the two trailing padding bytes from this four-byte
	 * RGBA payload. */
	result = command("Ga=T,f=32,s=1,v=1,i=56,C=1;/wAA/w", line_a);
	ASSERT(result.redraw);
	ASSERT_EQ(1, (int)graphics_image_count());
	ASSERT_EQ(4, (int)graphics_image_bytes());
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT_EQ(255, last_draw.rgba[0]);
	ASSERT_EQ(255, last_draw.rgba[3]);
}

TEST(kitten_icat_large_single_apc)
{
	GraphicsCommandResult result;
	unsigned char *raw;
	char *encoded, *sequence;
	size_t raw_length = 1025 * 4;
	size_t encoded_capacity = ((raw_length + 2) / 3) * 4 + 1;
	size_t sequence_capacity = encoded_capacity + 128;

	reset_state();
	raw = malloc(raw_length);
	encoded = malloc(encoded_capacity);
	sequence = malloc(sequence_capacity);
	ASSERT(raw != NULL);
	ASSERT(encoded != NULL);
	ASSERT(sequence != NULL);
	memset(raw, 0x7f, raw_length);
	base64(raw, raw_length, encoded);
	ASSERT(strlen(encoded) > 4096);
	snprintf(sequence, sequence_capacity,
	    "Ga=T,q=2,f=32,s=1025,v=1,i=57,C=1;%s", encoded);
	graphics_handle_apc(sequence, strlen(sequence), line_a, 2, 0, 10, 20,
	    24, line_row, 1, &result);
	ASSERT(result.redraw);
	ASSERT_EQ(1, (int)graphics_image_count());
	ASSERT_EQ((int)raw_length, (int)graphics_image_bytes());
	free(sequence);
	free(encoded);
	free(raw);
}

TEST(zlib_compressed_raw_rgba)
{
	unsigned char raw[] = {1, 2, 3, 4, 5, 6, 7, 8};
	unsigned char compressed[64];
	uLongf compressed_length = sizeof(compressed);
	char encoded[128], sequence[256];
	reset_state();
	ASSERT_EQ(Z_OK, compress2(compressed, &compressed_length, raw,
	    sizeof(raw), Z_BEST_COMPRESSION));
	base64(compressed, compressed_length, encoded);
	snprintf(sequence, sizeof(sequence),
	    "Ga=T,f=32,s=2,v=1,o=z,i=66,C=1;%s", encoded);
	command(sequence, line_a);
	graphics_draw(0, GRAPHICS_STAGE_ABOVE_TEXT, 10, 20, 24,
	    line_row, capture_draw, NULL);
	ASSERT_EQ(1, drawn);
	ASSERT_EQ(1, last_draw.rgba[0]);
	ASSERT_EQ(4, last_draw.rgba[3]);
	ASSERT_EQ(5, last_draw.rgba[4]);
	ASSERT_EQ(8, last_draw.rgba[7]);
}

TEST(image_count_limit_evicts_and_allows_replacement)
{
	char sequence[128];
	reset_state();
	for (int i = 1; i <= 1024; i++) {
		snprintf(sequence, sizeof(sequence),
		    "Ga=t,f=24,s=1,v=1,i=%d,q=2;AAAA", i);
		command(sequence, line_a);
	}
	ASSERT_EQ(1024, (int)graphics_image_count());
	command("Ga=t,f=24,s=1,v=1,i=1024,q=2;AQID", line_a);
	ASSERT_EQ(1024, (int)graphics_image_count());
	command("Ga=t,f=24,s=1,v=1,i=1025,q=2;AAAA", line_a);
	ASSERT_EQ(1024, (int)graphics_image_count());
	ASSERT(freed_images >= 2);
}

TEST(malformed_interleaving_aborts_chunk_transfer)
{
	reset_state();
	command("Ga=T,f=32,s=1,v=1,i=99,m=1;/wAA", line_a);
	command("Gthis-is-not-valid", line_a);
	command("Gm=0;/w==", line_a);
	ASSERT_EQ(0, (int)graphics_image_count());
	ASSERT_EQ(0, (int)graphics_placement_count());
}

TEST_SUITE(graphics)
{
	RUN_TEST(query_direct_rgb);
	RUN_TEST(chunked_rgba_placement);
	RUN_TEST(natural_size_and_cursor_movement);
	RUN_TEST(negative_z_stage_and_reanchor);
	RUN_TEST(fully_offscreen_placement_is_culled);
	RUN_TEST(recycle_removes_placement_not_addressed_image);
	RUN_TEST(reflow_updates_anchor_and_column);
	RUN_TEST(clear_buffer_keeps_reusable_data);
	RUN_TEST(hard_delete_all_reclaims_visible_image_data);
	RUN_TEST(position_and_z_delete_selectors);
	RUN_TEST(unsupported_transport_and_quiet_errors);
	RUN_TEST(invalid_payload_does_not_allocate);
	RUN_TEST(png_query);
	RUN_TEST(png_pixels_compact_and_restore);
	RUN_TEST(selected_placement_copies_cropped_png_atomically);
	RUN_TEST(kitten_icat_unpadded_base64);
	RUN_TEST(kitten_icat_large_single_apc);
	RUN_TEST(zlib_compressed_raw_rgba);
	RUN_TEST(image_count_limit_evicts_and_allows_replacement);
	RUN_TEST(malformed_interleaving_aborts_chunk_transfer);
	graphics_reset();
}

int
main(void)
{
	printf("graphics test suite\n");
	RUN_SUITE(graphics);
	return test_summary();
}

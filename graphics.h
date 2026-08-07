/* Kitty terminal graphics protocol and backend-neutral image state. */
#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

#include "st.h"

#define GRAPHICS_STAGE_BELOW_BACKGROUND 0
#define GRAPHICS_STAGE_BELOW_TEXT       1
#define GRAPHICS_STAGE_ABOVE_TEXT       2
#define GRAPHICS_HEADER_MAX             2048U
#define GRAPHICS_DATA_MAX        (64U * 1024U * 1024U)
/* A direct-transfer payload is base64, so its wire representation can be up
 * to four thirds the size of the bounded decoded image data.  Although Kitty
 * recommends 4096-byte chunks, clients such as kitten icat also emit a whole
 * image in one APC. */
#define GRAPHICS_PAYLOAD_MAX (((GRAPHICS_DATA_MAX + 2U) / 3U) * 4U)
#define GRAPHICS_APC_MAX \
	(1U + GRAPHICS_HEADER_MAX + 1U + GRAPHICS_PAYLOAD_MAX)

typedef struct {
	int handled;
	int redraw;
	int move_cursor;
	int columns;
	int rows;
	char response[512];
	size_t response_len;
} GraphicsCommandResult;

typedef struct {
	uint64_t serial;
	uint32_t image_id;
	uint32_t placement_id;
	const unsigned char *rgba;
	int image_width;
	int image_height;
	int source_x;
	int source_y;
	int source_width;
	int source_height;
	int column;
	int row;
	int columns;
	int rows;
	int pixel_x;
	int pixel_y;
	int natural_size;
	int z;
} GraphicsPlacementView;

typedef void (*GraphicsDrawCallback)(const GraphicsPlacementView *, void *);
typedef void (*GraphicsImageFreeCallback)(uint64_t, void *);
typedef Line (*GraphicsReflowLineAt)(void *, long long);

int graphics_handle_apc(const char *data, size_t length, Line anchor,
		int column, int alt, int cell_width, int cell_height,
		int viewport_rows, int (*line_to_row)(Line, int), int available,
		GraphicsCommandResult *result);
void graphics_draw(int alt, int stage, int cell_width, int cell_height,
		int viewport_rows,
		int (*line_to_row)(Line, int), GraphicsDrawCallback draw,
		void *context);
void graphics_recycle_line(Line line);
void graphics_reanchor_line(Line oldline, Line newline);
void graphics_reanchor_line_address(uintptr_t oldline, Line newline);
void graphics_reflow_line(Line oldline, int logical_offset, int new_columns,
		long long first_output, GraphicsReflowLineAt line_at, void *context);
int graphics_line_extent(Line line);
void graphics_clear_buffer(int alt);
void graphics_reset(void);
void graphics_set_image_free_callback(GraphicsImageFreeCallback callback,
		void *context);
size_t graphics_image_bytes(void);
size_t graphics_image_count(void);
size_t graphics_placement_count(void);
const unsigned char *graphics_image_pixels(uint64_t serial);
void graphics_release_image_pixels(uint64_t serial);
void graphics_compact_images(void);
int graphics_has_visible_placements(int alt, int viewport_rows,
		int (*line_to_row)(Line, int));

#endif

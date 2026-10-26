/* Shared per-cell compositing for positive-z Kitty images. */
#ifndef ST_IMAGE_OCCLUSION_H
#define ST_IMAGE_OCCLUSION_H

#include "../graphics.h"

#define IMAGE_OCCLUSION_CELL_MAX (1024U * 1024U)

typedef struct ImageBaseline ImageBaseline;
typedef struct {
	ImageBaseline *baselines;
	size_t cells;
} ImageOcclusion;

typedef void (*ImageOcclusionDraw)(Glyph, int, int, void *);

/* Grid cells an image may draw on: rows [top, end), columns [left, right). */
typedef struct {
	int top, end, left, right;
} ImageOcclusionClip;

/* Call begin/end once per completed frame, even when no images are visible.
 * Baselines follow Line identity through scrollback and are bounded separately
 * from decoded image memory. A selected image passes a NULL draw callback so
 * it remains one atomic visual object; that keeps its baseline alive but never
 * snapshots cells, which may carry the application's selection decoration. */
void image_occlusion_begin(ImageOcclusion *state);
void image_occlusion_end(ImageOcclusion *state);
void image_occlusion_clear(ImageOcclusion *state);
void image_occlusion_draw(ImageOcclusion *state,
		const GraphicsPlacementView *placement, int columns, int rows,
		Line (*line_at)(int), ImageOcclusionDraw draw, void *context);
/* Like image_occlusion_draw() for an image clipped to `clip` (an image clip
 * region, OSC 2032 ; k): only the cells it can draw over are remembered and
 * composited. When the clip changes, the cells both clips reach keep what
 * was remembered there and the ones newly in reach are taken as they are,
 * so cells the image never covered (a prompt below the pane it is clipped
 * to) never hide it, and an overlay drawn over it as the clip changes does. */
void image_occlusion_draw_clipped(ImageOcclusion *state,
		const GraphicsPlacementView *placement, int columns, int rows,
		Line (*line_at)(int), ImageOcclusionDraw draw, void *context,
		const ImageOcclusionClip *clip);

#endif

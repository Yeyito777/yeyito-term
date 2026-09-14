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

/* Call begin/end once per completed frame, even when no images are visible.
 * Baselines follow Line identity through scrollback and are bounded separately
 * from decoded image memory. A selected image keeps its baseline but passes a
 * NULL draw callback so it remains one atomic visual object. */
void image_occlusion_begin(ImageOcclusion *state);
void image_occlusion_end(ImageOcclusion *state);
void image_occlusion_clear(ImageOcclusion *state);
void image_occlusion_draw(ImageOcclusion *state,
		const GraphicsPlacementView *placement, int columns, int rows,
		Line (*line_at)(int), ImageOcclusionDraw draw, void *context);

#endif

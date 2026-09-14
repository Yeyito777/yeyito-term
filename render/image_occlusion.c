#include <stdlib.h>

#include "image_occlusion.h"

/* Positive-z images remember the original terminal cell beneath each part of
 * their footprint. Keep that protocol-controlled cache bounded independently
 * of decoded image data so a client cannot grow renderer memory indefinitely. */
typedef struct {
	Line line;
	int column;
	Glyph glyph;
} ImageBaselineCell;

struct ImageBaseline {
	struct ImageBaseline *next;
	uint64_t serial;
	int seen;
	uint32_t image_id;
	uint32_t placement_id;
	Line anchor;
	int alt;
	int column;
	int columns;
	int rows;
	int source_x;
	int source_y;
	int source_width;
	int source_height;
	int pixel_x;
	int pixel_y;
	int natural_size;
	ImageBaselineCell *cells;
	size_t celllen;
	size_t cellcap;
};

void
image_occlusion_begin(ImageOcclusion *state)
{
	ImageBaseline *baseline;
	for (baseline = state->baselines; baseline; baseline = baseline->next)
		baseline->seen = 0;
}

void
image_occlusion_end(ImageOcclusion *state)
{
	ImageBaseline **link, *baseline;

	for (link = &state->baselines; (baseline = *link); ) {
		if (baseline->seen) {
			link = &baseline->next;
			continue;
		}
		*link = baseline->next;
		state->cells -= baseline->celllen;
		free(baseline->cells);
		free(baseline);
	}
}

static int
baseline_equal(const ImageBaseline *baseline,
		const GraphicsPlacementView *placement)
{
	return baseline->image_id == placement->image_id &&
	    (placement->image_id || baseline->serial == placement->serial) &&
	    baseline->placement_id == placement->placement_id &&
	    baseline->anchor == placement->anchor &&
	    baseline->alt == placement->alt &&
	    baseline->column == placement->column &&
	    baseline->columns == placement->columns &&
	    baseline->rows == placement->rows &&
	    baseline->source_x == placement->source_x &&
	    baseline->source_y == placement->source_y &&
	    baseline->source_width == placement->source_width &&
	    baseline->source_height == placement->source_height &&
	    baseline->pixel_x == placement->pixel_x &&
	    baseline->pixel_y == placement->pixel_y &&
	    baseline->natural_size == placement->natural_size;
}

static ImageBaseline *
baseline_get(ImageOcclusion *state, const GraphicsPlacementView *placement)
{
	ImageBaseline *baseline;

	for (baseline = state->baselines; baseline; baseline = baseline->next)
		if (baseline_equal(baseline, placement)) {
			baseline->seen = 1;
			return baseline;
		}
	baseline = calloc(1, sizeof(*baseline));
	if (!baseline)
		return NULL;
	baseline->serial = placement->serial;
	baseline->seen = 1;
	baseline->image_id = placement->image_id;
	baseline->placement_id = placement->placement_id;
	baseline->anchor = placement->anchor;
	baseline->alt = placement->alt;
	baseline->column = placement->column;
	baseline->columns = placement->columns;
	baseline->rows = placement->rows;
	baseline->source_x = placement->source_x;
	baseline->source_y = placement->source_y;
	baseline->source_width = placement->source_width;
	baseline->source_height = placement->source_height;
	baseline->pixel_x = placement->pixel_x;
	baseline->pixel_y = placement->pixel_y;
	baseline->natural_size = placement->natural_size;
	baseline->next = state->baselines;
	state->baselines = baseline;
	return baseline;
}

static size_t
baseline_hash(Line line, int column, size_t capacity)
{
	uint64_t value = (uint64_t)(uintptr_t)line >> 4;
	value ^= (uint32_t)column * UINT64_C(11400714819323198485);
	value ^= value >> 33;
	value *= UINT64_C(0xff51afd7ed558ccd);
	value ^= value >> 33;
	return (size_t)value & (capacity - 1);
}

static int
baseline_resize(ImageBaseline *baseline, size_t capacity)
{
	ImageBaselineCell *cells;
	size_t i, slot;

	cells = calloc(capacity, sizeof(*cells));
	if (!cells)
		return 0;
	for (i = 0; i < baseline->cellcap; i++) {
		if (!baseline->cells[i].line)
			continue;
		slot = baseline_hash(baseline->cells[i].line,
		    baseline->cells[i].column, capacity);
		while (cells[slot].line)
			slot = (slot + 1) & (capacity - 1);
		cells[slot] = baseline->cells[i];
	}
	free(baseline->cells);
	baseline->cells = cells;
	baseline->cellcap = capacity;
	return 1;
}

static ImageBaselineCell *
baseline_cell(ImageOcclusion *state, ImageBaseline *baseline, Line line, int column,
		Glyph glyph)
{
	ImageBaselineCell *cell;
	size_t slot;

	if (baseline->cellcap) {
		slot = baseline_hash(line, column, baseline->cellcap);
		while (baseline->cells[slot].line) {
			cell = &baseline->cells[slot];
			if (cell->line == line && cell->column == column)
				return cell;
			slot = (slot + 1) & (baseline->cellcap - 1);
		}
	}
	if (state->cells >= IMAGE_OCCLUSION_CELL_MAX)
		return NULL;
	if (!baseline->cellcap ||
	    (baseline->celllen + 1) * 2 >= baseline->cellcap) {
		if (!baseline_resize(baseline,
		    baseline->cellcap ? baseline->cellcap * 2 : 16))
			return NULL;
	}
	slot = baseline_hash(line, column, baseline->cellcap);
	while (baseline->cells[slot].line)
		slot = (slot + 1) & (baseline->cellcap - 1);
	cell = &baseline->cells[slot];
	cell->line = line;
	cell->column = column;
	cell->glyph = glyph;
	baseline->celllen++;
	state->cells++;
	return cell;
}

static int
glyph_equal(Glyph left, Glyph right)
{
	return left.u == right.u && left.mode == right.mode &&
	    left.fg == right.fg && left.bg == right.bg;
}

void
image_occlusion_draw(ImageOcclusion *state,
		const GraphicsPlacementView *placement, int columns, int rows,
		Line (*line_at)(int), ImageOcclusionDraw draw, void *context)
{
	ImageBaseline *baseline;
	ImageBaselineCell *cell;
	Line line;
	int x, y, firstx, lastx, firsty, lasty;

	if (placement->z < 0)
		return;
	baseline = baseline_get(state, placement);
	if (!baseline)
		return;
	firstx = MAX(0, placement->column);
	lastx = MIN(columns, placement->column + placement->columns);
	firsty = MAX(0, placement->row);
	lasty = MIN(rows, placement->row + placement->rows);
	for (y = firsty; y < lasty; y++) {
		line = line_at(y);
		if (!line)
			continue;
		for (x = firstx; x < lastx; x++) {
			/* Snapshot every covered grid cell independently. The original
			 * glyph/background stays below the positive-z image; only a changed
			 * cell is composited above it. Restoring that exact cell reveals only
			 * the corresponding part of the image again. */
			cell = baseline_cell(state, baseline, line, x, line[x]);
			if (!cell || glyph_equal(cell->glyph, line[x]))
				continue;
			if (!draw)
				continue;
			if ((line[x].mode & ATTR_WDUMMY) && x > 0) {
				draw(line[x - 1], x - 1, y, context);
				continue;
			}
			draw(line[x], x, y, context);
		}
	}
}

void
image_occlusion_clear(ImageOcclusion *state)
{
	image_occlusion_begin(state);
	image_occlusion_end(state);
}

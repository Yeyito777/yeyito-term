#include "test.h"
#include "../render/image_occlusion.h"

static Glyph grid[4][8];
static Line lines[4];
static int drawn[4][8], count;
static Glyph lastGlyph;
static ImageOcclusion state;
static GraphicsPlacementView placement;

static Line
line_at(int y)
{
	return lines[y];
}

static void
record_cell(Glyph glyph, int x, int y, void *context)
{
	(void)context;
	drawn[y][x]++;
	lastGlyph = glyph;
	count++;
}

static void
setup(void)
{
	image_occlusion_clear(&state);
	for (int y = 0; y < 4; y++) {
		lines[y] = grid[y];
		for (int x = 0; x < 8; x++)
			grid[y][x] = (Glyph){.u = ' ', .fg = 7, .bg = 0};
	}
	placement = (GraphicsPlacementView){.serial = 1, .image_id = 1,
	    .placement_id = 1, .anchor = grid[1], .column = 1, .row = 1,
	    .columns = 4, .rows = 2, .source_width = 40, .source_height = 20};
}

static void
frame(int selected)
{
	memset(drawn, 0, sizeof(drawn));
	count = 0;
	image_occlusion_begin(&state);
	image_occlusion_draw(&state, &placement, 8, 4, line_at,
	    selected ? NULL : record_cell, NULL);
	image_occlusion_end(&state);
}

TEST(menu_cells_occlude_and_restore_independently)
{
	setup();
	frame(0);
	ASSERT_EQ(0, count);
	ASSERT_EQ(8, state.cells);
	grid[1][1].u = '/';
	grid[1][2].u = 'm';
	grid[2][3].bg = 4; /* blank menu background also occludes */
	grid[0][1].u = 'X'; /* outside image */
	frame(0);
	ASSERT_EQ(3, count);
	ASSERT_EQ(1, drawn[1][1]);
	ASSERT_EQ(1, drawn[1][2]);
	ASSERT_EQ(1, drawn[2][3]);
	grid[1][1].u = ' ';
	frame(0);
	ASSERT_EQ(2, count);
	ASSERT_EQ(0, drawn[1][1]);
	grid[1][2].u = ' ';
	grid[2][3].bg = 0;
	frame(0);
	ASSERT_EQ(0, count);
}

TEST(erased_text_and_style_changes_occlude)
{
	setup();
	grid[1][1].u = 'x';
	frame(0);
	grid[1][1].u = ' '; /* default-bg blank must paint over image */
	grid[1][2].fg = 3;
	grid[1][3].mode = ATTR_REVERSE;
	frame(0);
	ASSERT_EQ(3, count);
}

TEST(selection_preserves_baseline)
{
	setup();
	frame(0);
	grid[1][1].u = '/';
	frame(1);
	ASSERT_EQ(0, count);
	ASSERT_EQ(8, state.cells);
	frame(0);
	ASSERT_EQ(1, count);
}

TEST(negative_z_never_occludes)
{
	setup();
	placement.z = -1;
	frame(0);
	grid[1][1].u = '/';
	frame(0);
	ASSERT_EQ(0, count);
	ASSERT_EQ(0, state.cells);
	ASSERT_NULL(state.baselines);
}

TEST(retransmit_keeps_named_image_baseline)
{
	setup();
	frame(0);
	grid[1][1].u = '/';
	placement.serial++;
	frame(0);
	ASSERT_EQ(1, count);
	ASSERT_EQ(8, state.cells);
	placement.placement_id++;
	frame(0);
	ASSERT_EQ(0, count);
	ASSERT_EQ(8, state.cells);
}

TEST(anonymous_images_use_serial)
{
	setup();
	placement.image_id = 0;
	frame(0);
	grid[1][1].u = '/';
	placement.serial++;
	frame(0);
	ASSERT_EQ(0, count);
}

TEST(scroll_follows_line_identity)
{
	setup();
	frame(0);
	grid[1][1].u = '/';
	lines[0] = grid[1];
	lines[1] = grid[2];
	placement.row = 0;
	frame(0);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[0][1]);
	ASSERT_EQ(8, state.cells);
}

TEST(wide_dummy_redraws_leading_cell)
{
	setup();
	/* The leading cell lies outside the image footprint. */
	placement.column = 2;
	grid[1][1].u = 0x4e00;
	grid[1][1].mode = ATTR_WIDE;
	frame(0);
	grid[1][2].mode = ATTR_WDUMMY;
	frame(0);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[1][1]);
	ASSERT_EQ(0x4e00, lastGlyph.u);
}

TEST(clipping_and_missing_lines)
{
	setup();
	placement.column = -1;
	placement.row = -1;
	placement.columns = 12;
	placement.rows = 8;
	lines[2] = NULL;
	frame(0);
	ASSERT_EQ(24, state.cells);
	grid[3][7].u = '/';
	frame(0);
	ASSERT_EQ(1, count);
}

TEST(deletion_prunes_baselines)
{
	setup();
	frame(0);
	image_occlusion_begin(&state);
	image_occlusion_end(&state);
	ASSERT_EQ(0, state.cells);
	ASSERT_NULL(state.baselines);
	grid[1][1].u = '/';
	frame(0);
	ASSERT_EQ(0, count);
	image_occlusion_clear(&state);
	ASSERT_EQ(0, state.cells);
	ASSERT_NULL(state.baselines);
}

TEST(cache_limit_does_not_prevent_existing_cell_occlusion)
{
	setup();
	frame(0);
	size_t saved = state.cells;
	state.cells = IMAGE_OCCLUSION_CELL_MAX;
	grid[1][1].u = '/';
	lines[2] = grid[3]; /* new cells cannot be snapshotted at the limit */
	frame(0);
	ASSERT_EQ(1, count);
	ASSERT_EQ(IMAGE_OCCLUSION_CELL_MAX, state.cells);
	state.cells = saved;
}

int
main(void)
{
	RUN_TEST(menu_cells_occlude_and_restore_independently);
	RUN_TEST(erased_text_and_style_changes_occlude);
	RUN_TEST(selection_preserves_baseline);
	RUN_TEST(negative_z_never_occludes);
	RUN_TEST(retransmit_keeps_named_image_baseline);
	RUN_TEST(anonymous_images_use_serial);
	RUN_TEST(scroll_follows_line_identity);
	RUN_TEST(wide_dummy_redraws_leading_cell);
	RUN_TEST(clipping_and_missing_lines);
	RUN_TEST(deletion_prunes_baselines);
	RUN_TEST(cache_limit_does_not_prevent_existing_cell_occlusion);
	image_occlusion_clear(&state);
	return test_summary();
}

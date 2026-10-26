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

TEST(selected_placement_does_not_snapshot_decoration)
{
	/* A TUI re-places a thumbnail with V=1 while its cursor-line highlight
	 * paints one covered row (after a viewer deleted and restored it). The
	 * highlight must not become the baseline: once the cursor leaves, the
	 * plain row is the image's original background, not an occluding change. */
	setup();
	frame(0);
	image_occlusion_begin(&state); /* viewer open: placement absent */
	image_occlusion_end(&state);
	ASSERT_NULL(state.baselines);
	for (int x = 0; x < 8; x++)
		grid[2][x].bg = 4;
	grid[2][3].mode = ATTR_REVERSE; /* inline history cursor */
	frame(1);
	ASSERT_EQ(0, count);
	ASSERT_EQ(0, state.cells);
	ASSERT_NOT_NULL(state.baselines);
	for (int x = 0; x < 8; x++)
		grid[2][x].bg = 0;
	grid[2][3].mode = 0;
	frame(0);
	ASSERT_EQ(0, count);
	ASSERT_EQ(8, state.cells);
	/* The baseline taken after deselection is the plain row, so a later
	 * decoration without selection still occludes as a menu would. */
	grid[2][1].bg = 4;
	frame(0);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[2][1]);
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

static void
frame_clipped(const ImageOcclusionClip *clip)
{
	memset(drawn, 0, sizeof(drawn));
	count = 0;
	image_occlusion_begin(&state);
	image_occlusion_draw_clipped(&state, &placement, 8, 4, line_at,
	    record_cell, NULL, clip);
	image_occlusion_end(&state);
}

/* An image clipped to a region (a chat pane, OSC 2032 ; k) placed whole
 * past it: only the cells it draws over count.  A prompt row below the
 * pane changing never hides the image, and when the clip grows the cells
 * newly in reach are remembered as they are then. */
TEST(clip_region_limits_snapshot_and_refreshes)
{
	ImageOcclusionClip pane = { .top = 0, .end = 2, .left = 0, .right = 8 };
	ImageOcclusionClip taller = { .top = 0, .end = 3, .left = 0, .right = 8 };

	setup();	/* the image covers rows 1-2, columns 1-4 */
	frame_clipped(&pane);
	ASSERT_EQ(4, state.cells);	/* row 1 only */
	grid[2][2].u = '>';	/* the prompt below the pane */
	frame_clipped(&pane);
	ASSERT_EQ(0, count);
	grid[1][2].u = '/';	/* a menu inside the pane still occludes */
	frame_clipped(&pane);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[1][2]);
	grid[1][2].u = ' ';
	/* The pane grows over row 2: its cells as they are now are the image's. */
	frame_clipped(&taller);
	ASSERT_EQ(0, count);
	ASSERT_EQ(8, state.cells);
	grid[2][3].u = 'x';
	frame_clipped(&taller);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[2][3]);
	/* Unclipped, the cells remembered under the clip still count: the 'x'
	 * written over the image keeps hiding it. */
	frame(0);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[2][3]);
}

/* The clip changes in the frame an overlay opens (Record drops its pane's
 * clip while its image viewer is open): the cells both clips cover keep the
 * image's own cells, so the overlay's title written over the image in that
 * same frame still hides it; only cells newly in reach are taken as they are. */
TEST(clip_change_keeps_cells_both_clips_cover)
{
	ImageOcclusionClip pane = { .top = 0, .end = 2, .left = 0, .right = 8 };

	setup();	/* the image covers rows 1-2, columns 1-4 */
	grid[2][2].u = '>';	/* the prompt below the pane */
	frame_clipped(&pane);
	ASSERT_EQ(0, count);
	grid[1][2].u = 't';	/* the overlay's title, over the image */
	frame(0);	/* ... and the clip is gone */
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[1][2]);
	ASSERT_EQ(0, drawn[2][2]);	/* newly in reach: as it is now */
	ASSERT_EQ(8, state.cells);
	/* And back: the title still covers it, now clipped again. */
	frame_clipped(&pane);
	ASSERT_EQ(1, count);
	ASSERT_EQ(1, drawn[1][2]);
	grid[1][2].u = ' ';	/* the overlay closes */
	frame_clipped(&pane);
	ASSERT_EQ(0, count);
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
	RUN_TEST(selected_placement_does_not_snapshot_decoration);
	RUN_TEST(negative_z_never_occludes);
	RUN_TEST(retransmit_keeps_named_image_baseline);
	RUN_TEST(anonymous_images_use_serial);
	RUN_TEST(scroll_follows_line_identity);
	RUN_TEST(wide_dummy_redraws_leading_cell);
	RUN_TEST(clipping_and_missing_lines);
	RUN_TEST(clip_region_limits_snapshot_and_refreshes);
	RUN_TEST(clip_change_keeps_cells_both_clips_cover);
	RUN_TEST(deletion_prunes_baselines);
	RUN_TEST(cache_limit_does_not_prevent_existing_cell_occlusion);
	image_occlusion_clear(&state);
	return test_summary();
}

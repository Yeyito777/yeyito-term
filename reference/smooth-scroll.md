# Smooth scrolling

Pixel-smooth scrolling of

- **the scrollback view**: trackpads scroll by pixels with the system's
  momentum; mouse wheels and keyboard scrolls (Shift+PageUp, nav-mode
  `Ctrl+u`/`Ctrl+d`, `gg`/`G`, search jumps, snap-to-prompt) glide to their
  target in about 100 ms;
- **application scroll regions**: when a full-screen app (neovim, less, the
  Exocortex/Record TUIs) scrolls a DECSTBM region, the region's content glides
  by those rows while status lines, tab lines and command lines stay put.

At rest the view always sits on whole rows.

## Config (`config.h` / `config.def.h`)

```c
static int smoothscroll = 1;          /* 0 = everything off */
static double smoothscroll_ms = 100;  /* glide duration */
static int smoothscroll_regions = 1;  /* glide app scroll regions */
```

`smoothscroll_ms = 0` keeps trackpad pixel scrolling but makes every other
scroll jump. `smoothscroll_regions = 0` stops animating application regions
unless the app itself asks for it with DECSCLM (below).

### DECSCLM (`CSI ? 4 h` / `CSI ? 4 l`)

The historical "smooth scroll" mode is an application opt-in: `CSI ? 4 h`
turns region animation on even when `smoothscroll_regions = 0`. `CSI ? 4 l`
only withdraws that request; it never disables the configured default,
because routine terminal reset strings send it. A terminal reset (RIS) clears
it. With `smoothscroll = 0`, DECSCLM does nothing.

## Turning it off / reverting

- **Off, keep the code:** set `smoothscroll = 0` in `config.h` and rebuild.
  Every hook then returns immediately: no offsets, no extra rows, no clips, no
  timers. Scrolling and drawing behave as on `main`, and trackpads use the old
  line-based path. `smoothscroll_regions = 0` turns off only the region part.
- **Revert completely:** the feature lives on the `smooth-scroll` branch and
  was never merged. `git checkout main` and rebuild (on macOS also
  `make install-app`, or restore the backed-up `st.app`).

## Design

The terminal grid and its logical scroll position (`term.scr`, whole rows)
still change immediately, exactly as without the feature. Smooth scrolling is
purely a presentation layer: for each grid row the renderer asks how far to
shift it (a fraction of rows, decaying to 0) and what band to clip it to.
Selection, search highlights, vimnav cursor/line highlight, Kitty images and
the cursor are all keyed by grid rows, so they move with their rows and can
never drift out of alignment. What is drawn is always the current grid:
rows the app rewrites mid-glide show their new content at the current offset.

Positive shifts draw rows lower; negative shifts draw them higher.

### Pieces

| File | Role |
|------|------|
| `render/smoothscroll.c/.h` | Pure engine (unit tested): ease curve, clamping, visible rows, snapping, device-pixel rounding, region heuristics (`ss_region_animates`), render-path choice, and the state machine `IDLE / ANIMATING / PRECISE`. |
| `render/smoothscroll_term.c` | Terminal glue: scrollback following, region state (band, copies of departing lines, snapshot + static-column classification), per-cell API for renderers (`smoothscroll_shift/clip/span/plain/band`), precise input, DECSCLM. |
| `st.c` | `tviewline()/tscrolled()/thistlen()` accessors; `smoothscroll_frame()` at the top of `draw()`, `smoothscroll_drawextra()` after `drawregion()`; `smoothscroll_region()` at the start of `tscrollup()/tscrolldown()`; DECSCLM in `tsetmode()`; reset in `treset()`. |
| `graphics.c` | `graphics_draw_rows()` culls placements against the visible row range (partial rows included). |
| `search.c` | Uncached-row sentinel is `INT_MIN` (row `-1` is now drawable). |
| `x.c`, `render/gpu.c` | `xdrawline()` splits band rows at the moving span. GL: shift + clip in `gpubatchquad()` per cell segment (`gpubegincell()`), images by anchor cell (vertex shift + scissor). Xft: per-segment offset (`xrowdy()`) and Xft clip rectangle (`xrowclip()`). Run-loop frame pacing, mouse rows. |
| `macos/renderer.m/.h`, `macos/backend.m` | Metal: `mac_renderer_set_row()` shifts and clips quads and image vertices (UV-correct) per cell (`beginCell()`); `xdrawline()` segments; trackpad phases, display-link pacing, mouse rows. |

### Scrollback

`smoothscroll_frame()` runs at the start of every `draw()`. If `term.scr`
changed since the last frame, it finds where the line that was on row 0 now
sits (`r`) and adds `-r` rows to the shift, so the old content is first drawn
exactly where it was, then glides to rest (cubic ease-out over
`smoothscroll_ms`). This catches every scroll source without hooking each one.
Every row gets the same shift, clipped to the grid; rows just outside the
viewport are drawn from the scrollback.

- Output arriving while scrolled up bumps `term.scr` but keeps the row-0 line
  in place (`r = 0`): never animated. Output at the bottom does not change
  `term.scr`: never animated.
- Jumps further than a screen glide in from one screen away.
- The shift is clamped so nothing past the oldest history line or below the
  live screen is ever revealed.
- Alternate screen, alt-screen switches and resizes reset it to 0.

### Application scroll regions

`tscrollup()`/`tscrolldown()` call `smoothscroll_region(top, bot, orig, n)`
before moving rows. Every scroll path goes through them: SU/SD (`CSI n S/T`),
IND/LF at the bottom margin, RI at the top margin, and IL/DL (`CSI n L/M`).

**What neovim emits** (v0.12, `TERM=st-256color`): it never uses SU/SD. To
scroll a window it sets the region to the window's text rows, homes the cursor
to the region top and deletes/inserts lines there, then restores the full
region and draws the newly exposed lines:

```
C-e     ESC[1;22r ESC[H ESC[M    ESC[1;24r ESC[22;1H <new line> ...
C-y     ESC[1;22r ESC[H ESC[L    ESC[1;24r ESC[H <new line> ...
C-d     ESC[1;22r ESC[H ESC[11M  ESC[1;24r ESC[12;1H <11 new lines> ...
C-f     ESC[1;22r ESC[H ESC[20M  ...
wheel   ESC[1;22r ESC[H ESC[3M   ...
G, gg   no scroll: rows are repainted in place
```

so IL/DL at the top margin count as region scrolls, and neovim glides with
no configuration.

**When a region scroll animates** (`ss_region_animates`):

- `orig` is the region's top margin. SU/SD/IND/RI always are; IL/DL only when
  the cursor is on the top margin. IL/DL further down is an edit (insert or
  delete a line inside the region), so it jumps.
- The region has at least 3 rows (1-2 row regions are tickers/status lines).
- The scroll moves fewer rows than the region has (more is a redraw).
- Alternate screen, or the primary screen with a partial region. Full-screen
  scrolls on the primary screen are ordinary shell output and never animate.
- The scrollback is at the bottom and not gliding.
- Scrolls accumulating past a whole region within one glide (fast output)
  snap instead of lagging behind.

**Motion:** the band's rows shift by `+n` rows (drawn where they were) and
glide to 0, clipped to the band; rows outside it never move. The lines that
left the band are copied before the scroll and drawn as "plain" lines (no
selection/search/line highlights) sliding out over the edge, so the motion
looks exactly like the scrollback glide. New rows appear from the other edge
as the app draws them.

**Side panels (static columns):** st has no left/right margins (DECLRMM), so
TUIs with side panels scroll the whole width and repaint the panels in place,
usually in the same synchronized update (the Exocortex TUI: `CSI t;b r`,
`CSI n S`, `CSI r`, then the sidebar columns of the shifted rows). Each batch
of scrolls snapshots the band first. When the batch is presented, and again
on later frames of the glide (for panels repainted late), every column votes
per cell: *stayed* (equals the snapshot at the same row), *moved* (equals the
snapshot at its source row) or *rewritten* (neither). Cells where stayed and
moved predict the same thing (blank areas, text repeated on every row) don't
vote. Only rows that scrolled vote: a row with more *rewritten* cells than
*moved* ones (a cursor-line highlight that stays on its screen row, text
re-wrapped beside a floating panel) would otherwise pin every column where it
matches its old self, and longer lines of other rows would tear there. A
column moves when *moved* wins; the moving span runs from the first to
the last such column. Non-voting neighbours join it up to the row edge, or, in
a run between moving and static columns, up to the widest all-blank gap, so a
pane's repeated prefix moves with the pane while a panel's border and labels
stay with the panel. Everything outside the span (sidebars, re-wrapped
right-hand panels) is drawn in place; departing-line copies are drawn only in
the span. If the moving columns were mostly rewritten rather than scrolled, the
band does not animate at all. Line drawers split band rows into segments at
the span (`smoothscroll_span()`), and cells shift/clip by
`smoothscroll_shift(x, y)` / `smoothscroll_clip(x, y, ...)`.

DECLRMM (`CSI ? 69 h` + DECSLRM) is not implemented: it would change every
scroll, insert/delete and wrap path in the core, and the detection above lets
apps keep scrolling full-width bands.

**Accumulation:** scrolls before the next presented frame add up without the
clock running, including a whole synchronized update (mode 2026: frames are
withheld, and the glide starts when the batch is presented). A scroll arriving
mid-glide retargets from the current position, as for scrollback; copies
coming back from the other edge are dropped because the app redraws them as
grid rows. A scroll of a different band snaps the current glide and starts
the new one; a redraw-sized scroll of the gliding band snaps it.

**Kitty images:** placements are anchored to lines, and region scrolls move
lines, so an image moves with its anchor row's shift and is clipped to the
band. Images on lines that left the band are gone (as without the feature).

**Trackpad in the alternate screen:** with mouse reporting the wheel still
goes to the app unchanged; the glide comes from the app's resulting scrolls.

### Precise (trackpad) input — macOS

`scrollWheel:` with `hasPreciseScrollingDeltas` (and no Shift, no mouse
reporting) calls `smoothscroll_pixels(scrollingDeltaY, cellHeight, held)`. The
shift follows the fingers 1:1; whenever it passes half a row, the logical view
scrolls a row and the shift is compensated, so it stays within ±0.5 rows.
Gesture phases decide `held`; momentum events keep the gesture alive. 60 ms
after the last event with fingers up, the view snaps to the nearest row and
glides there. Touching the trackpad mid-glide catches it. In the alternate
screen or with smooth scrolling off, the old line-based path is used.

Linux has no precise path yet: X11 core events only deliver discrete wheel
buttons (XInput2 smooth-scroll valuators would be the way to add it). Wheel
and keyboard scrolls glide.

### Rendering

| Platform | Renderer | How a row is shifted and clipped |
|----------|----------|----------------------------------|
| macOS | Metal (always; the native backend has no non-Metal renderer and exits if Metal is missing) | `beginCell(x, y)` sets `mac_renderer_set_row(dy, clip, rect)`; `quad()` and image vertices add `dy` and clip to the rectangle with matching UV adjustment. Window UI (command line, toasts, SSH badge, IME text) never sets a cell, so it never moves. Shifts are rounded to device pixels. |
| Linux, GPU | OpenGL via GLX (`gpudraw = 1`, the default) | `gpubegincell(x, y)` sets the segment's pixel shift and clip rectangle; `gpubatchquad()` applies them (UV-correct). Images shift by their anchor cell and use a band scissor. Full clear each moving frame (retained damage tracking is bypassed while moving and for one frame at rest). |
| Linux, CPU fallback | Xft into the backing pixmap (`gpudraw = 0`, or GLX unavailable) | The moving band is cleared, then each segment is drawn `xrowdy(x, y)` pixels lower under an Xft clip rectangle. Kitty images need the GPU renderer, as before. |

"GPU on Linux" means the existing fixed-function OpenGL renderer in
`render/gpu.c`. On machines without a GPU, Mesa runs it in software
(llvmpipe), which is still the GL path. The Xft path is the true CPU fallback.

The X11 command line, SSH badge and toasts are separate child windows and
never move.

### Frame pacing and idle cost

- X11: `run()` schedules frames at ~120 Hz only while `smoothscroll_pending()`
  (GLX vsync may pace them further). It is re-armed right after `draw()`,
  because that is where a new scroll is noticed.
- macOS: `MTKView` runs its display link (`preferredFramesPerSecond = 120`)
  only while a glide or snap is pending, then returns to on-demand drawing.
- At rest nothing is scheduled: zero idle CPU/GPU. Typing never waits on the
  animation. The region hook costs a few compares per scroll; ordinary shell
  output on the primary screen is rejected on the first checks.

### Mouse

Mouse rows subtract the scrollback shift (`evrow()` on X11, `eventRow()` on
macOS), so clicks/selection during a scrollback glide land on the row under
the pointer. Region glides don't remap the mouse (events go to the app, and
the grid is already final).

## Tests

- `tests/test_smoothscroll.c` (in `make test`): engine math and state
  machine; the scrollback glue against a fake scrollback with a stepped clock
  (disabled = inert, glides, output never animates, alternate-screen bypass,
  big jumps, precise follow/hold/settle, buffer-end clamps, resize); regions
  (heuristics, band-only shift and clip, departing-line copies in both
  directions, sync-batch accumulation, mid-glide retarget and reversal,
  jump cases, edits mid-glide, knob + DECSCLM, inert when disabled, screen
  switch reset); static columns (sidebar repainted in a sync batch stays put
  and departing lines are drawn only in the pane, re-wrapped right panel stays
  put, mostly rewritten band does not glide, late panel repaint refines the
  span, a pane's repeated prefix moves with the pane, a panel's repeated label
  stays with the panel, a rewritten highlight row does not pin columns, a band
  of rewritten rows does not glide, uniform bands glide full width).
- `tests/test_graphics.c`: `graphics_draw_rows()` includes partial rows.
- `tests/test_macos_image_occlusion.m` (macOS): offscreen Metal pixels — a
  region scroll shifts its band's cells, image and cursor, clips at the band
  edge, leaves the status row and window UI in place.

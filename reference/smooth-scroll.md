# Smooth scrolling

Pixel-smooth scrolling of the scrollback view. Trackpads scroll by pixels
with the system's momentum; mouse wheels and keyboard scrolls (Shift+PageUp,
nav-mode `Ctrl+u`/`Ctrl+d`, `gg`/`G`, search jumps, snap-to-prompt) glide to
their target in about 100 ms. At rest the view always sits on whole rows.

## Config (`config.h` / `config.def.h`)

```c
static int smoothscroll = 1;          /* 0 = off */
static double smoothscroll_ms = 100;  /* glide duration for wheel/keys */
```

Both platforms read the same knobs. `smoothscroll_ms = 0` keeps trackpad
pixel scrolling but makes wheel/keyboard scrolls jump as before.

## Turning it off / reverting

- **Off, keep the code:** set `smoothscroll = 0` in `config.h` and rebuild.
  Every hook then returns immediately: no offsets, no extra rows, no clip,
  no timers. Scrolling and drawing behave as on `main`, and trackpads use the
  old line-based path.
- **Revert completely:** the feature lives on the `smooth-scroll` branch and
  was never merged. `git checkout main` and rebuild (on macOS also
  `make install-app`, or restore the backed-up `st.app`).

## Design

The terminal's logical scroll position (`term.scr`, whole rows) still changes
immediately, exactly as without the feature. Smooth scrolling is purely a
presentation layer: a fractional row **offset** by which the renderer
translates the grid, plus the partial rows that translation exposes. Because
selection, search highlights, vimnav cursor/line highlight, Kitty images and
the cursor are all keyed by grid rows, they move with the grid and can never
drift out of alignment.

Positive offsets draw the grid lower (exposing older history above row 0);
negative offsets draw it higher (exposing rows below the last one).

### Pieces

| File | Role |
|------|------|
| `render/smoothscroll.c/.h` | Pure engine (no terminal/platform deps, unit tested): ease curve, clamping, visible rows, snapping, device-pixel rounding, mouse row mapping, render-path choice, and the state machine `IDLE / ANIMATING / PRECISE`. |
| `render/smoothscroll_term.c` | Terminal glue: follows `term.scr` changes from `draw()`, drives the engine, draws the exposed partial rows, exposes the pixel offset to renderers, handles precise input. |
| `st.c` | `tviewline()/tscrolled()/thistlen()` accessors; `smoothscroll_frame()` at the top of `draw()` and `smoothscroll_drawextra()` after `drawregion()`. |
| `graphics.c` | `graphics_draw_rows()` culls placements against the visible row range (partial rows included). |
| `search.c` | Uncached-row sentinel is `INT_MIN` (row `-1` is now drawable). |
| `x.c`, `render/gpu.c` | X11 GL and Xft renderers, run-loop frame pacing, mouse row mapping. |
| `macos/renderer.m/.h`, `macos/backend.m` | Metal translation + scissor, UI-overlay split, trackpad phases, display-link pacing. |

### Following scrolls

`smoothscroll_frame()` runs at the start of every `draw()`. If `term.scr`
changed since the last frame, it finds where the line that was on row 0 now
sits (`r`) and adds `-r` rows to the offset, so the old content is first drawn
exactly where it was, then glides to rest (cubic ease-out over
`smoothscroll_ms`). This catches every scroll source without hooking each one.

- Output arriving while scrolled up bumps `term.scr` but keeps the row-0 line
  in place (`r = 0`): never animated. Output at the bottom does not change
  `term.scr`: never animated.
- Jumps further than a screen glide in from one screen away.
- The offset is clamped so nothing past the oldest history line or below the
  live screen is ever revealed.
- Alternate screen, alt-screen switches and resizes reset the offset to 0.

### Precise (trackpad) input — macOS

`scrollWheel:` with `hasPreciseScrollingDeltas` (and no Shift, no mouse
reporting) calls `smoothscroll_pixels(scrollingDeltaY, cellHeight, held)`. The
offset follows the fingers 1:1; whenever it passes half a row, the logical view
scrolls a row and the offset is compensated, so it stays within ±0.5 rows.
Gesture phases decide `held`; momentum events keep the gesture alive. 60 ms
after the last event with fingers up, the view snaps to the nearest row and
glides there. Touching the trackpad mid-glide catches it. In the alternate
screen or with smooth scrolling off, the old line-based path is used.

Linux has no precise path yet: X11 core events only deliver discrete wheel
buttons (XInput2 smooth-scroll valuators would be the way to add it). Wheel
and keyboard scrolls glide.

### Rendering

| Platform | Renderer | How the offset is applied |
|----------|----------|---------------------------|
| macOS | Metal (always; the native backend has no non-Metal renderer and exits if Metal is missing) | Vertex-shader translation (`view.zw` uniform) of grid layers, images and the cursor; scissor to the band between the borders; overlay vertices after `mac_renderer_mark_ui_overlay()` (command line, toasts, SSH badge, IME text) are drawn untranslated with a full scissor. Offset rounded to device pixels. |
| Linux, GPU | OpenGL via GLX (`gpudraw = 1`, the default) | `glTranslatef` on the modelview matrix around all grid batches and image quads, `glScissor` to the grid band, full clear each moving frame (retained damage tracking is bypassed while moving and for one frame at rest). |
| Linux, CPU fallback | Xft into the backing pixmap (`gpudraw = 0`, or GLX unavailable) | Rows drawn `xssoff` pixels lower into the pixmap under an Xft clip rectangle after clearing the grid area; then the usual `XCopyArea`. Kitty images need the GPU renderer, as before. |

"GPU on Linux" here means the existing fixed-function OpenGL renderer in
`render/gpu.c`. On machines without a GPU, Mesa runs it in software
(llvmpipe), which is still the GL path. The Xft path is the true CPU fallback.
A new GPU path was not needed: the GL renderer already rebuilds whole frames
and a modelview translate costs nothing.

Exposed rows outside `[0, rows)` are drawn with the normal `xdrawline()` at
their own (negative or `>= rows`) row numbers, so selection/search/line
highlight logic applies to them unchanged.

The X11 command line, SSH badge and toasts are separate child windows and
never move. On macOS they are drawn after the UI-overlay mark.

### Frame pacing and idle cost

- X11: `run()` schedules frames at ~120 Hz only while `smoothscroll_pending()`
  (GLX vsync may pace them further). It is re-armed right after `draw()`,
  because that is where a new scroll is noticed.
- macOS: `MTKView` runs its display link (`preferredFramesPerSecond = 120`)
  only while a glide or snap is pending, then returns to on-demand drawing.
- At rest nothing is scheduled: zero idle CPU/GPU. Typing never waits on the
  animation; at the bottom of the buffer the hook is a couple of integer
  compares per frame.

### Mouse

Mouse rows subtract the current pixel offset (`evrow()` on X11, `eventRow()`
on macOS), so clicks/selection during a glide land on the row under the
pointer. At rest the offset is 0.

## Tests

- `tests/test_smoothscroll.c` (in `make test`): engine math and state
  machine, plus the terminal glue against a fake scrollback with a stepped
  clock: disabled = inert, glide + exposed rows both directions, output never
  animates, alternate-screen bypass, big jumps, precise follow/hold/settle,
  buffer-end clamps, resize reset, render-path (CPU fallback) choice.
- `tests/test_graphics.c`: `graphics_draw_rows()` includes partial rows.
- `tests/test_macos_image_occlusion.m` (macOS): offscreen Metal pixels — grid,
  image and cursor move together, the UI overlay stays put, the band clips.

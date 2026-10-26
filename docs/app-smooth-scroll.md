# App-driven smooth scrolling (mode 2032)

Full-screen applications (alternate screen) can animate their own scrolling
in st with sub-row (pixel) offsets. The application decides what moves and
when; st only draws the moving rectangles shifted and keeps the lines its
scrolls pushed out. Because the application keeps laying its content out one
whole row at a time, text it re-wraps (beside a floating panel, say) re-wraps
as it glides.

Without this, st scrolls full-screen applications instantly (see
`smoothscroll_regions` in `reference/smooth-scroll.md`). Guessing which cells
of an application's region scroll moved and which stayed put cannot get side
panels and re-wrapped text right, so st no longer guesses unless configured to.

The Exocortex TUI uses this (`tui/src/smoothscroll.ts`). st times the glide
itself at the display's refresh rate; the application sends one short
sequence per input and per row it lays out, so pty or ssh jitter never shows
as judder and the traffic stays tiny even when the application runs remotely.

## Sequences

| Sequence | Meaning |
| --- | --- |
| `CSI ? 2032 $ p` | DECRQM: does st support it? |
| `CSI ? 2032 ; Ps $ y` | Reply: `1` set, `2` reset (supported), `4` permanently reset (`smoothscroll = 0`). Terminals without it do not reply, or reply `0`. |
| `CSI ? 2032 h` | Turn app-driven smooth scrolling on. |
| `CSI ? 2032 l` | Turn it off (also clears everything below). RIS turns it off. |
| `OSC 2032 ; r ; T,L,B,R ; T,L,B,R … ST` | The moving rectangles: rows `T`..`B`, columns `L`..`R`, 1-based and inclusive, at most 8. Replaces the previous set; no rectangles moves nothing. |
| `OSC 2032 ; m ; dA ; B ; D ST` | Move: the offset's base changes by `dA`, and an easing term of `B` starts now and eases out to 0 over `D` ms (st's cubic ease-out). `B` and `D` may be left out. Values in thousandths of a row; positive draws the content lower. |
| `OSC 2032 ; o ; N ST` | Set the base to `N` and drop every easing term (start over, no animation). `0` is at rest. |
| `OSC 2032 ; c ST` | Clear: offset 0, no rectangles, no clip regions, no kept lines. |
| `OSC 2032 ; k ; T,L,B,R ; T,L,B,R … ST` | Image clip regions (same form as `r`, at most 8): an image whose anchor cell lies in one never draws outside the clip regions on its anchor row, at rest or while gliding. Replaces the previous set; no regions clears it. See [Image clip regions](#image-clip-regions). |
| `CSI ? 2033 $ p` | DECRQM: does st support image clip regions? |
| `CSI ? 2033 ; Ps $ y` | Reply: `3` permanently set (supported; nothing to turn on, `k` works while mode 2032 is on), `4` permanently reset (`smoothscroll = 0`). Terminals without it do not reply, or reply `0`. |

`OSC` may end with `ST` (`ESC \`) or `BEL`.

## What st draws

The offset at a frame is the base plus what each easing term still has left
at that frame's time, so st evaluates it once per refresh of the display the
window is on (X11: the monitor's mode from XRandR; macOS: the screen's
`maximumFramesPerSecond`, ProMotion up to 120 Hz) and draws frames only while
a term is running. It never goes past the lines st kept for that side (see
below): beyond them the offset is held at that many rows.

While the offset `o` is not 0, every grid cell inside a rectangle is drawn `o`
rows lower (or higher), clipped to its rectangle. An image moves with the
offset and clips to the bounding box of every rectangle on its anchor row (an
image may span rectangles, such as the history beside a side panel and below
it), never to just one of them. At rest nothing moves or clips: cells and
images draw exactly as without the mode, so an application placing an image
past a rectangle to have it while gliding must crop it again at rest, unless it
declares [image clip regions](#image-clip-regions). Cells outside every
rectangle (a sidebar, a panel, status lines, the cursor's prompt) never move.
Images anchored in a rectangle move with it. Selection, search highlights and
the cursor are keyed by grid cells, so they move with their cells.

The strip a shifted rectangle uncovers at its top (o > 0) or bottom (o < 0)
shows the lines st's own scrolls pushed out of the scroll region, when that
edge of the rectangle is the edge of the region, nearest line first. Past
those, or for an edge inside the region (a rectangle starting below a panel),
it is filled with the background of the row just beyond the edge, so a glide
never uncovers bare terminal background.

st keeps those lines while mode 2032 is on: every scroll of a region (`CSI n
S`/`T`, IND/LF at the bottom margin, RI at the top margin, IL/DL with the
cursor on the top margin) copies the lines it pushes out, up to the region's
height, and drops the copies on the other side (the application redraws what
comes back in). Images (Kitty placements) anchored on a line that leaves go
with its copy, cropped to the rows that left; the application placing the
image again inside the region does not take them back. A scroll of a different
region, of the whole region or more, a resize and a screen switch drop the
copies; so does coming to rest (offset 0, no easing left). Automatic region
glides are off while the mode is on.

## How an application animates

1. Query with DECRQM at startup; on `1` or `2`, send `CSI ? 2032 h` (and
   `CSI ? 2032 l` on exit).
2. Keep the same curve st keeps: offset = base + easing terms, and the eased
   position = laid-out row `g` + offset. An input that moves the target by
   `s` rows is one `OSC 2032 ; m ; s ; -s ; 100`: the target moves, the new
   term cancels it at first, so nothing jumps, then eases out. Inputs that
   arrive mid-glide add up the same way.
3. Lay the content out at a whole row slightly ahead of the position in the
   direction of travel (the Exocortex TUI looks 30 ms ahead, but never more
   than the row just past the position), so st's own clock does not reach a
   row the application has not laid out, while the offset stays within about
   a row (content laid out further ahead than that is drawn far from where it
   was laid out, which shows wherever the application re-wraps text): scrolling
   back (content moving down) the layout is lower than the position and the
   offset negative, so the strip is at the bottom and shows lines the scroll
   pushed out of the bottom; scrolling forward the reverse.
4. Each time the layout moves by `k` rows, send one synchronized update
   (mode 2026): the region scroll (`CSI t;b r`, `CSI k S`/`T`, `CSI r`), the
   rows it exposed or that re-wrap, the rectangles if they changed, and
   `OSC 2032 ; m ; -k` (content up) or `; k` (down) so the position does
   not move. Nothing is sent between rows.
5. When a frame cannot scroll (it had to be repainted, or jumped further than
   the region), start over: `OSC 2032 ; o ; T−g`, `OSC 2032 ; m ; 0 ; −(T−g) ; 100`
   eases from the repainted rows to the target `T`.

Content that moves by itself (output appended while following the end) can
glide the same way: scroll the region, then `OSC 2032 ; m ; 0 ; k ; 100` for
the `k` rows it moved.

### Images

An image crossing an edge of the region keeps showing in the strip the glide
uncovers. When it leaves through the top, st keeps the rows that left on the
copied lines. When it leaves through the bottom its anchor is still inside
the region, so the application places it uncropped below the region's last
row (the rectangles clip it while they move).

At rest nothing clips, so without clip regions the application must crop such
an image again before the glide ends, and st's clock runs behind the
application's by the trip through the pty or ssh: crop late and st draws the
image over whatever lies below the pane (a prompt) until the crop arrives;
crop early and the image's last pixels are missing for the end of the glide.
Clip regions remove that race.

### Image clip regions

`OSC 2032 ; k` declares the rectangles images may draw in, typically the
pane the history scrolls in. An image whose anchor cell lies in a clip region
is clipped, every frame, to the bounding box of the clip regions on its
anchor row (a pane split by a side panel is two regions; an image below the
panel spans both). At rest that is the only clip; while gliding it also
clips to the moving rectangles, as without clip regions. So the application
places an image cut off by the pane's edge uncropped past it, always, and
never crops it again at rest: st never draws it outside the pane. Images
outside every clip region (a sidebar's icons, a prompt's emoji, an image
viewer anchored outside the pane) behave exactly as before, and text cells
are never clipped.

Clip regions are part of mode 2032: they take effect only while it is on and
are cleared with the rest by `CSI ? 2032 l`, RIS, `OSC 2032 ; c`, a resize
and a screen switch (send them again with the next frame). Hiding an image
whose cells change after it was drawn (st's image occlusion,
[kitty-graphics.md](kitty-graphics.md)) then considers only the cells the
image can draw over inside its clip regions: rows below the pane that an
uncropped image never draws over never hide it. When a placement's clip
changes (or goes), the cells both clips reach keep what was remembered there,
so an overlay drawn over the image in the frame its clip changes (an image
viewer opening as the application drops its pane's clip) still hides it, and
the cells newly in reach are taken as they are.

Detect support with DECRQM `CSI ? 2033 $ p`; a reply of `3` means `k` is
understood. Mode 2033 is not used by any terminal known to us (the nearby
2026-2031 and 2048 are synchronized output, Contour's extensions and in-band
resize). Kitty images are drawn by the GPU renderers (X11 GL and macOS
Metal), which both clip; the Xft fallback draws no Kitty images.

### Panels and re-wrapped text

st has no left/right margins (DECLRMM), so a region scroll moves whole rows,
panels included; the application repaints its panels in the same synchronized
update, as it does without this mode. Rectangles say what actually moves. A
pane with a panel in its top-right corner, with the pane's text re-wrapping
beside it, is two rectangles that share the offset: the pane's columns left of
the panel for the full height, and the panel's columns from the first row
below it. Text in the first rectangle glides and re-wraps a row at a time;
text in the second slides under the panel's bottom edge.

### Synchronization, resize, cancellation

- Inside a synchronized update the offset and rectangles take effect when the
  update is presented, together with the scroll and the redrawn rows.
- A resize or screen switch clears the offset, rectangles and kept lines (the
  mode stays on); the application sends them again with its next frame.
- To cancel, send `OSC 2032 ; o ; 0` (or `OSC 2032 ; c`).
- `ST_SMOOTHSCROLL_LOG=file` makes st log every frame it draws while
  something moves (pid, time in ms, scrollback / region / app offsets in
  rows, rows scrolled so far) to check pacing. A terminal already running
  appends each glide to `$TMPDIR/st-smoothscroll.log` (or `/tmp/...`) while
  that file exists: create it, scroll, read it, delete it.
  `ST_REFRESH_HZ` overrides the refresh rate st detects (testing).

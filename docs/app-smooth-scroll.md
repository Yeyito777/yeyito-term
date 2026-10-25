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

The Exocortex TUI uses this (`tui/src/smoothscroll.ts`); the sequences are
small enough to send at every frame even when the application runs remotely.

## Sequences

| Sequence | Meaning |
| --- | --- |
| `CSI ? 2032 $ p` | DECRQM: does st support it? |
| `CSI ? 2032 ; Ps $ y` | Reply: `1` set, `2` reset (supported), `4` permanently reset (`smoothscroll = 0`). Terminals without it do not reply, or reply `0`. |
| `CSI ? 2032 h` | Turn app-driven smooth scrolling on. |
| `CSI ? 2032 l` | Turn it off (also clears everything below). RIS turns it off. |
| `OSC 2032 ; r ; T,L,B,R ; T,L,B,R … ST` | The moving rectangles: rows `T`..`B`, columns `L`..`R`, 1-based and inclusive, at most 8. Replaces the previous set; no rectangles moves nothing. |
| `OSC 2032 ; o ; N ST` | The offset of every rectangle: `N` thousandths of a row, positive draws the content lower. `0` is at rest. |
| `OSC 2032 ; c ST` | Clear: offset 0, no rectangles, no kept lines. |

`OSC` may end with `ST` (`ESC \`) or `BEL`.

## What st draws

While the offset `o` is not 0, every grid cell inside a rectangle is drawn `o`
rows lower (or higher), clipped to its rectangle. Cells outside every
rectangle (a sidebar, a panel, status lines, the cursor's prompt) never move.
Images anchored in a rectangle move with it. Selection, search highlights and
the cursor are keyed by grid cells, so they move with their cells.

The strip a shifted rectangle uncovers at its top (o > 0) or bottom (o < 0)
shows the lines st's own scrolls pushed out of the scroll region, when that
edge of the rectangle is the edge of the region, nearest line first. Otherwise
the strip is background.

st keeps those lines while mode 2032 is on: every scroll of a region (`CSI n
S`/`T`, IND/LF at the bottom margin, RI at the top margin, IL/DL with the
cursor on the top margin) copies the lines it pushes out, up to the region's
height, and drops the copies on the other side (the application redraws what
comes back in). A scroll of a different region, of the whole region or more, a
resize and a screen switch drop them; so does an offset of 0. Automatic region
glides are off while the mode is on.

## How an application animates

1. Query with DECRQM at startup; on `1` or `2`, send `CSI ? 2032 h` (and
   `CSI ? 2032 l` on exit).
2. Keep a continuous scroll position `p` that eases towards the target
   (st's own glides: cubic ease-out over 100 ms, `smoothscroll_ms`).
3. Lay the content out at a whole row `g` next to `p`, one step ahead in the
   direction of travel, and set `o` to the difference in rows (positive
   down). Scrolling back (content moving down, `CSI n T`) the layout is
   already lower than `p`, so `o` lies in (−1, 0]: drawn higher, the strip is
   at the bottom and holds the line the scroll pushed out of the bottom.
   Scrolling forward (content moving up, `CSI n S`) `o` lies in [0, 1) and
   the strip at the top holds the line pushed out of the top.
4. Each time `g` changes, send one synchronized update (mode 2026): the region
   scroll (`CSI t;b r`, `CSI n S`/`T`, `CSI r`), the rows it exposed or that
   re-wrap, the rectangles if they changed, and `OSC 2032 ; o ; …`. Between
   rows send only the offset (about 15 bytes).
5. Finish with `OSC 2032 ; o ; 0`.

Content that moves by itself (output appended while following the end) can
glide the same way: scroll the region, then set the offset to the distance it
moved and let it decay.

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
- To cancel, send offset 0 (or `OSC 2032 ; c`). Offsets beyond what st kept
  show background in the strip; keep `|o|` below the region height.

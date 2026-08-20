# Kitty graphics architecture

`st` implements a backend-neutral subset of the
[Kitty terminal graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/).
The implementation is centered on direct image transmission because the escape
sequence and its image bytes travel through a PTY and SSH connection together.
It never asks a remote application to name a file or shared-memory object on the
computer running the terminal.

## Source layout

The protocol and image lifecycle are shared by every renderer. Only texture
creation and drawing are platform-specific.

| Path | Responsibility |
| --- | --- |
| `graphics.c`, `graphics.h` | Protocol parsing, transfer assembly, decoding, image and placement state, scrollback anchoring, reflow, culling, and the renderer-neutral API. |
| `st.c` | Recognizes graphics APC strings, bounds their size, dispatches commands, applies cursor movement, and connects placements to terminal lines and history. |
| `macos/backend.m` | Supplies native pixel/cell geometry and translates visible placements into Metal renderer calls. |
| `macos/renderer.m`, `macos/renderer.h` | Cache RGBA data as `MTLTexture` objects and composite image layers with the terminal grid. |
| `x.c` | Supplies X11/OpenGL geometry and integrates graphics into frame drawing. |
| `render/gpu.c` | Cache RGBA data as OpenGL textures and draw the image layers. |
| `vendor/stb_image.h` | Vendored PNG decoder, compiled in PNG-only mode. |
| `tests/test_graphics.c` | Protocol, decoding, placement, deletion, culling, reflow, selection export, and quota tests. |

The common code communicates with a renderer using `GraphicsPlacementView` and
callbacks declared in `graphics.h`. It has no Cocoa, Metal, X11, or OpenGL
dependency.

## End-to-end data flow

1. An application writes a Kitty graphics APC string to its terminal. The
   7-bit wire form starts with bytes `1b 5f 47` (`ESC _ G`) and ends with
   `1b 5c` (`ESC` followed by a backslash).
2. `st.c` collects the control string. Graphics strings are limited to a
   2,048-byte control header and enough base64 payload for the 64 MiB decoded
   transfer limit. An oversized APC is discarded through its string terminator
   rather than grown without bound or interpreted as terminal text. Clients
   should use 4,096-byte chunks, but bounded single-APC transfers and unpadded
   base64 are accepted for compatibility with `kitten icat`.
3. `graphics_handle_apc()` parses the command. If `m=1` is present, decoded
   chunks are accumulated until a final `m=0` command arrives. A malformed or
   interleaved command aborts the partial transfer.
4. Direct data is optionally inflated with zlib and decoded as RGB, RGBA, or
   PNG. All successful inputs are normalized to an owned, 8-bit RGBA buffer.
5. A transmit command creates or replaces a `GraphicsImage`. A display command
   additionally creates a `GraphicsPlacement` anchored at the current terminal
   `Line` and column.
6. At frame time, `graphics_draw()` finds placements intersecting the current
   viewport, sorts them by z-index, image ID, and creation serial, and emits a
   `GraphicsPlacementView` through the active backend callback.
7. The Metal or OpenGL backend looks up a texture by the image's internal
   serial number, uploads it on demand, and draws the requested source rectangle
   into the placement rectangle. On Linux, texture residency is limited to
   images intersecting the current viewport.
8. When common image state is reclaimed, an image-free callback immediately
   invalidates the corresponding GPU texture.

The internal serial is separate from the protocol image ID. It changes when an
application replaces an image, which prevents a renderer from accidentally
reusing stale texture contents under the same protocol ID.

## Supported protocol subset

### Transmission and image data

- Direct transport: `t=d`
- Formats: 24-bit RGB (`f=24`), 32-bit RGBA (`f=32`), and PNG (`f=100`)
- Optional zlib compression: `o=z`
- Multi-command transfers: `m=1` followed by a final `m=0`
- Image IDs (`i`), image numbers (`I`), and placement IDs (`p`)
- Quiet levels `q=0`, `q=1`, and `q=2`

The implemented actions are transmit (`a=t`), transmit and display (`a=T`),
query (`a=q`), place an existing image (`a=p`), and delete (`a=d`). Capability
queries decode and validate their data but do not retain it.

### Placement

Placements support:

- Cell dimensions (`c`, `r`) or natural pixel sizing
- Source cropping (`x`, `y`, `w`, `h`)
- Pixel offsets inside the anchor cell (`X`, `Y`)
- Cursor movement control (`C`)
- Signed z-index (`z`)
- Alpha from RGBA and PNG input

Deletion selectors `a`, `i`, `n`, `c`, `p`, `q`, `x`, `y`, `z`, and `r` are
implemented. Uppercase selectors perform hard deletion: after matching
placements are removed, image data with no remaining placements is reclaimed.

### Deliberately unsupported features

- Local file, temporary-file, and shared-memory transports
- Animation frames
- Relative placements
- Kitty Unicode placeholders

Unsupported operations return a protocol error unless the selected quiet level
suppresses it. Applications should issue a Kitty graphics query instead of
inferring support from `$TERM`, which remains `st-256color`.

## Why direct transport works over SSH

SSH carries terminal output as bytes. A program on the remote host base64
encodes the image and writes the APC commands to its remote PTY; `st` receives
the same commands locally and performs the decode and GPU upload. No helper or
image file is needed on the local machine.

Tools that support Kitty's streaming mode can therefore be used directly in an
SSH shell:

```sh
kitten icat --transfer-mode=stream image.png
chafa --format kitty image.png
```

A minimal PNG sender looks like this:

```python
#!/usr/bin/env python3
import base64
import sys

data = base64.b64encode(open(sys.argv[1], "rb").read())
chunks = [data[i:i + 4096] for i in range(0, len(data), 4096)]

for index, chunk in enumerate(chunks):
    more = int(index + 1 < len(chunks))
    if index == 0:
        controls = f"a=T,t=d,f=100,i=1,m={more}".encode()
    else:
        controls = f"m={more}".encode()
    sys.stdout.buffer.write(b"\x1b_G" + controls + b";" + chunk + b"\x1b\\")
sys.stdout.buffer.flush()
```

Terminal multiplexers must pass the escape sequence through unchanged. tmux
can be configured for passthrough, but it does not natively retain these image
placements in its own scrollback model. A direct SSH shell is the fully
supported path.

## Placement lifetime and scrollback

A placement stores a pointer to its anchor `Line`, its anchor column, and
whether it belongs to the normal or alternate screen. `st` moves the allocated
line itself into history while scrolling, so the pointer continues to identify
the same text and image location without copying image state.

Lifecycle hooks keep those pointers valid:

- `graphics_recycle_line()` removes placements when a line allocation is about
  to be reused or freed.
- `graphics_reanchor_line()` updates anchors when line contents move to a new
  allocation.
- `graphics_reflow_line()` converts the old logical-line offset plus placement
  column into both a new line anchor and a new column after a width change.
- `graphics_line_extent()` makes image-only or textually blank rows participate
  in reflow.
- `graphics_clear_buffer()` removes placements for the selected screen while
  retaining addressed image data that an application can place again.

Normal and alternate-screen placements are kept separate. Clearing or leaving
an alternate screen cannot make its placements appear on the normal screen.

## Rendering and compositing

Placements are divided into three passes:

1. Extremely negative z-index values are drawn below cell backgrounds.
2. Other negative values are drawn after backgrounds but before text.
3. Nonnegative values are drawn above terminal text.

Both renderers clip image draws to the terminal content rectangle, excluding
the window border. Vertically invisible placements are removed by the common
code before a texture lookup or upload; the backend also clips horizontally.
`tlineviewprepare()` builds a line-to-viewport-row lookup once per frame so
finding placement rows does not scan all scrollback for every image.

The Metal backend keeps one `MTLTexture` per live image serial and batches
placement geometry into the three image passes. On Retina displays it reports
backing-pixel dimensions, so an image using natural size maps image pixels to
physical display pixels.

The OpenGL backend uses a scissor rectangle for the terminal content area. It
keeps textures only for images drawn in the current frame, and releases the
entire texture set when dwm moves the terminal off-screen, X11 fully obscures it,
or the window is unmapped. Direct PNGs retain their compact encoded bytes in the
bounded common scrollback cache; decoded RGBA pixels are materialized only for
the duration of an on-demand texture upload. Scrolling an old image into view
decodes and uploads it again. This prevents image-heavy terminal history across
many windows from filling either VRAM or ordinary memory and stalling the X
server/window manager.
Graphics capability is reported only when the GPU renderer is available; the
non-GPU X11 path returns an unavailable response rather than claiming support it
cannot draw.

## Geometry and cursor behavior

The backend reports terminal pixel dimensions and cell pixel dimensions through
`xgetdimensions()`. In addition to the Kitty query response, `st` implements
the standard `CSI 14 t` and `CSI 16 t` geometry queries used by terminal image
clients.

Unless `C=1` is supplied, a displayed placement advances the terminal cursor as
a cell rectangle. This makes subsequent text begin after the placement in the
same way it would after occupying the requested rows and columns.

## Selection and clipboard export

A placement is an atomic visual object for terminal selections. Intersecting
any cell in its placement rectangle selects the complete source crop, including
when `V` selects an otherwise blank row below the placement's anchor. The Metal
and OpenGL backends tint the full displayed rectangle to make that atomic state
visible instead of relying on selection backgrounds hidden behind the image.

`getselimage()` asks the graphics store for the one placement intersected by the
current selection. The common code restores compacted PNG pixels on demand,
crops the normalized RGBA source, and emits a standalone RGBA PNG with zlib.
This also makes direct RGB/RGBA transfers and cropped placements independently
copyable. The X11 selection owner advertises `image/png` beside its UTF-8
targets and uses ICCCM `INCR` transfers for PNGs too large for one X request,
while the native backend publishes `NSPasteboardTypePNG`. Text-only consumers
receive `U+FFFC` for an image-only yank. A selection intersecting more than one
placement does not publish an arbitrary image because conventional system
clipboards have no portable ordered multi-image representation.

## Resource and parser limits

Terminal output is untrusted, especially when it comes from a remote host. The
implementation applies the following limits:

| Resource | Limit |
| --- | ---: |
| APC control header | 2,048 bytes |
| Base64 payload per APC | 89,478,488 bytes (64 MiB decoded) |
| Buffered transfer | 64 MiB |
| Normalized image | 64 MiB |
| Total normalized image memory | 320 MiB |
| Image dimensions | 8,192 x 8,192 pixels |
| Live images | 1,024 |
| Live placements | 1,024 |
| Placement width or height | 4,096 cells |

Before allocating raw image storage, dimensions and multiplication are checked
for overflow. PNG decoding is restricted to PNG input and the same dimension
limit. When total memory or image-count pressure is reached, the least recently
used image with no placement is evicted; visible images are not silently
discarded.

Offscreen placements remain in scrollback but are not uploaded or drawn until
they intersect the viewport.

## Testing

Run the graphics tests alone with:

```sh
make test_graphics
./tests/test_graphics
```

Or run the complete suite:

```sh
make test
```

The graphics tests cover direct and chunked input, RGB/RGBA/PNG decoding, zlib,
queries and quiet levels, cursor behavior, placement and deletion selectors,
scrollback recycling, resize reflow, viewport culling, malformed input, and
resource eviction.

# Native macOS backend

This directory contains the complete native macOS presentation layer. It does
not use XQuartz or link against X11.

- `backend.m` implements the AppKit application/window lifecycle, keyboard and
  mouse input, clipboard integration, terminal drawing adapter, overlays, and
  the local control socket used by the bundled helper scripts.
- `pasteboard5522.m` and `pasteboard5522.h` bridge native `NSPasteboard` image
  and text types to Kitty's authenticated OSC 5522 paste-event protocol. A
  single image file copied in Finder pastes the file's pixels, not the icon
  Finder puts beside it: PNG/JPEG/GIF/WebP up to 3 MB and 8000 px as they are,
  anything else (HEIC, TIFF, rotated photos) decoded upright with ImageIO and
  re-encoded as JPEG (PNG with alpha) within 2576 px and 3 MB. An image file
  that cannot be decoded, or several files, fail the read instead.
- `renderer.m` and `renderer.h` implement the Metal renderer, CoreText glyph
  atlas/font metrics, and cached RGBA textures for Kitty graphics placements.
- `glyph_layout.h` keeps color-font glyphs sized and centered by their visible
  ink bounds instead of the atlas padding or text baseline.
- `emoji.c` and `emoji.h` preserve macOS emoji-presentation selectors and their
  two-column terminal layout.
- `pty.m` and `pty.h` provide ordered, nonblocking PTY output. AppKit input
  handlers enqueue bytes here so a child that stops reading cannot freeze the
  window. PTY *input* (the child's output) is read in `backend.m` on its own
  dispatch queue into a 4 MiB ring and parsed on the main queue in batches
  (at most 8 ms at a time): a macOS PTY holds only about 1 KiB, so reading it
  on the main queue made every frame and key handled stall the child.
- `spare.c` and `spare.h` implement the warm spare's spawn-request parsing
  (and building) and single-slot protocol used by `st-aerospace-launch` (see
  `reference/warm-spare.md`).
- `launchlog.c` and `launchlog.h` format and append the launch diagnostics
  log written by `st-aerospace-launch` and by a claimed spare.
- `aerospace_launch.c` is `st-aerospace-launch`, a separate small program
  bundled in `Resources/bin`; `aerospace.c` and `aerospace.h` hold its
  testable parts: AeroSpace's socket protocol (framing, requests, events,
  answers) and the slot/venv helpers.
- `control.c` and `control.h` read one complete message from the per-instance
  control socket (`st-notify`, `st-save-cmd`, spare spawn requests).
- `native.h` is the small C bridge used by platform-neutral modules such as the
  command-line overlay.
- `keysyms.h` provides the minimal X11-compatible key values needed by shared
  key tables without introducing an X11 dependency.
- `Info.plist`, `st.icns`, and `st-icon.png` are the application-bundle assets.

`config.mk` defines `ST_NATIVE_MACOS` on Darwin, and the top-level `Makefile`
selects these Objective-C sources instead of `x.c`, `sshind.c`, and `notif.c`.
Linux continues to build the original X11/OpenGL backend.

## Frames and the frame timing log

Redraw requests (output, video frames, input) are coalesced to at most one
frame per display refresh; a glide or snap runs MTKView's display link at
the screen's refresh rate. The renderer lays out each rune with CoreText
once and keeps its glyphs, and puts a frame's vertices in one pooled buffer.

`ST_FRAME_LOG=FILE` logs every frame from launch. A running st logs while
`$TMPDIR/st-frames.log` exists (it looks once a second while drawing), so a
window already in use can be measured, and every st appends to that file
with its pid:

```sh
touch "$TMPDIR/st-frames.log"      # in any shell of the same user
# ... use st (scroll, type, play video) for a while ...
cp "$TMPDIR/st-frames.log" ~/st-frames.log && rm "$TMPDIR/st-frames.log"
scripts/st-frame-stats ~/st-frames.log      # per-process summary
```

Each line is `pid start since draw wait shown main tty_bytes parse flags`,
times in milliseconds: `start` is `CACurrentMediaTime()` when the frame
began, `since` the time from the previous frame's start, `draw` the main
thread's drawing time without `wait`, the time blocked for a drawable,
`shown` the time from start until the frame was on screen (-1 if it never
was), `main` the main thread's CPU time since the previous frame began,
`tty_bytes` and `parse` the PTY output parsed in between and the time that
took. `flags`: `g` a glide frame, `v` a video frame changed, `t` output
arrived, `k` a key was pressed, `w` the glide watchdog's timer drew it
instead of the display link (docs/app-smooth-scroll.md). A process stops logging
after 200,000 lines until the file is recreated.

`tests/macos_headless -A` measures the same things without a window (see
the comment at the top of `tests/macos_headless.m`).

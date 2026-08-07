# Native macOS backend

This directory contains the complete native macOS presentation layer. It does
not use XQuartz or link against X11.

- `backend.m` implements the AppKit application/window lifecycle, keyboard and
  mouse input, clipboard integration, terminal drawing adapter, overlays, and
  the local control socket used by the bundled helper scripts.
- `pasteboard5522.m` and `pasteboard5522.h` bridge native `NSPasteboard` image
  and text types to Kitty's authenticated OSC 5522 paste-event protocol.
- `renderer.m` and `renderer.h` implement the Metal renderer, CoreText glyph
  atlas/font metrics, and cached RGBA textures for Kitty graphics placements.
- `glyph_layout.h` keeps color-font glyphs sized and centered by their visible
  ink bounds instead of the atlas padding or text baseline.
- `emoji.c` and `emoji.h` preserve macOS emoji-presentation selectors and their
  two-column terminal layout.
- `pty.m` and `pty.h` provide ordered, nonblocking PTY output. AppKit input
  handlers enqueue bytes here so a child that stops reading cannot freeze the
  window.
- `native.h` is the small C bridge used by platform-neutral modules such as the
  command-line overlay.
- `keysyms.h` provides the minimal X11-compatible key values needed by shared
  key tables without introducing an X11 dependency.
- `Info.plist`, `st.icns`, and `st-icon.png` are the application-bundle assets.

`config.mk` defines `ST_NATIVE_MACOS` on Darwin, and the top-level `Makefile`
selects these Objective-C sources instead of `x.c`, `sshind.c`, and `notif.c`.
Linux continues to build the original X11/OpenGL backend.

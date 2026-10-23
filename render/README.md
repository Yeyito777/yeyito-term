# Renderer internals

This directory contains renderer-specific implementation that is intentionally
kept separate from the X11 event/input/window-management code in `x.c`.

`gpu.c` is included by `x.c` instead of being compiled as a separate translation
unit.  The GPU renderer depends on `x.c` private state such as window geometry,
Xft colors/fonts, and config globals.  Including it keeps those details private
while making the rendering code easier to find and review.

`smoothscroll.c` is the platform-neutral smooth-scroll engine and
`smoothscroll_term.c` its terminal glue (both compiled on Linux and macOS).
The GL renderer shifts and clips each row's quads in `gpubatchquad()`
(`gpubeginrow()`); see `../reference/smooth-scroll.md`.

Kitty graphics images use individual RGBA OpenGL textures rather than the glyph
atlases. Placements are emitted in three compositing stages: below cell
backgrounds, between backgrounds and text, and above text.

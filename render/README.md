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

`video_linux.c` is the Linux backend for st video (`a=v`, see
`../docs/video.md`): libavcodec decoder threads, PulseAudio output and the
playback clock.  A video placement goes through the same `gpudrawimage()`
path as an image (z stages, clips, glides, occlusion); `gpuvideotexture()`
uploads the current frame's Y, U and V planes only when the frame changes,
and a small shader converts them to RGB.  `run()` in `x.c` sleeps until the
next frame is due or a decoder signals its eventfd, and redraws only for
videos that were drawn in the last frame.

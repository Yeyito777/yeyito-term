# Renderer internals

This directory contains renderer-specific implementation that is intentionally
kept separate from the X11 event/input/window-management code in `x.c`.

`gpu.c` is included by `x.c` instead of being compiled as a separate translation
unit.  The GPU renderer depends on `x.c` private state such as window geometry,
Xft colors/fonts, and config globals.  Including it keeps those details private
while making the rendering code easier to find and review.

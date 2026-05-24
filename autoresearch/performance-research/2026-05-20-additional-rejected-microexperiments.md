# Additional rejected micro-experiments

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

This note records small experiments that were benchmarked and reverted during the
renderer research loop.  They did not warrant standalone commits because each was
short-lived and rejected, but keeping this summary avoids retrying the same ideas.

All patches listed here were reverted.  After reverts, the tree was rebuilt and
GPU regression tests were run successfully.

## Experiments

### Skip initial resize/reflow when startup geometry matches

Hypothesis: avoid the initial `cresize()`/`tresize_reflow()` work when `tnew()`
already created the requested geometry.

Result: startup A/B did not improve enough to matter and workload A/B was noisy or
slightly regressive.  Rejected.

### Skip old GPU cursor restore when old/new cursor cell are identical

Hypothesis: cursor overlays sometimes restore and redraw the same cell; skipping
that restore could reduce cursor-update overhead.

Result: direct A/B showed at most a tiny cursor wall improvement, while fair
GPU-vs-Xft cursor results remained poor and other metrics were noisy.  Rejected.

### Gate `search_invalidate_cache()` behind `search_active()`

Hypothesis: if search is inactive, avoid invalidating the search cache on every
draw.

Result: repaint sometimes improved, but scroll/cursor measurements regressed or
were too noisy.  Rejected.

### Uniform-color batch drawing

Hypothesis: for batches with a single color, use fixed `glColor3fv()` and disable
the color array to reduce per-vertex color traffic.

Result: some CPU metrics moved in the right direction, but cursor/repaint wall
were noisy or regressed.  Rejected.

### Deferred pending cursor-color updates

Hypothesis: queue cursor-color writes and apply the last update per cell before
draw, reducing repeated cell writes within one input burst.

Result: did not materially improve cursor performance and did not improve the
fair cursor blocker.  Rejected.

### Vertex color representation changes

Hypotheses tested:

- RGB floats instead of RGBA floats,
- packed `GL_UNSIGNED_BYTE` RGBA colors.

Results: both variants had some isolated wins, but fair cursor CPU regressed badly
and the changes did not clear the no-regression bar.  Rejected.

### Lazy atlas allocation variants

Hypothesis: delay allocating/uploading the main or color glyph atlas until the
first glyph upload to reduce startup/static costs.

Result: startup/static costs sometimes moved slightly, but cursor/repaint/scroll
benchmarks were noisy or regressive.  Emoji/Unicode atlas-capacity risk was not
worth it.  Rejected.

## Decision

Do not retry these variants in the same form.  The remaining blockers still appear
to be cursor-update and repaint work dominated by parser/write/draw interaction,
not simple GL state toggles or startup-only laziness.

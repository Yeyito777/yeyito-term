# Failed experiment: simple default-background row fast path

## Hypothesis

Most benchmark rows use the default background without selection/search/debug or
reverse-video effects. The normal GPU row renderer still checks/resolves the
background for every cell and builds a background run incrementally. A fast path
for rows whose cells all use the default background could emit one background
rectangle for the whole dirty span and then render glyphs/decorations, reducing
per-cell background comparison/copy work.

## Patch summary

In `render/gpu.c`, `gpudrawline()` was changed to:

- compute `simplebg` when selection/search/reverse/debug/current-vim-line effects
  are inactive,
- scan the dirty span for non-default backgrounds or explicit selected/match/reverse
  attrs,
- for simple rows, batch one default-background rectangle covering the full span,
  then draw text and decorations in a second loop,
- fall back to the existing renderer for all other rows.

This preserved the actual GPU renderer path, fractional scaling, glyph atlas and
color emoji behavior, and the accepted alpha-test/no-solid-blend states. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/simple-default-bg-line/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.745486`  
Relative score: `0.986x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6507 | 2.4559 | 1.8500 | 0.997x | 1.016x | 0.999x |
| repaint | 0.7375 | 1.4650 | 1.8295 | 0.984x | 1.015x | 0.999x |
| scroll_ascii | 0.9007 | 1.1141 | 1.8646 | 0.977x | 1.026x | 1.000x |
| scroll_unicode | 0.8861 | 1.1764 | 1.8264 | 0.977x | 1.049x | 0.999x |
| scroll_emoji | 1.0807 | 0.8299 | 1.8696 | 0.992x | 1.009x | 0.999x |

## Decision

Rejected and reverted.

The fast path added a second scan/loop and duplicated a large part of the glyph
rendering logic, which hurt code quality and measured performance. It regressed
the weighted score and both high-priority cursor/repaint workloads. The existing
single-pass background-run renderer remains better.

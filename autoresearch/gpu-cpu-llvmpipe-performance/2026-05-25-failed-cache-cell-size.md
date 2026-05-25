# Failed experiment: cache scaled GPU cell size

## Hypothesis

`gpucellw()` and `gpucellh()` recompute fractional-scale ratios from `win.w` /
`win.tw` and `win.h` / `win.th` each time they are called. Since the scaled cell
size only changes on resize/zoom, caching it in `Gpu` during `gpuresize()` could
avoid repeated divisions in hot row and cursor rendering while preserving
fractional-scaling behavior.

## Patch summary

In `render/gpu.c`, the experiment added cached `double cellw, cellh` fields to
`Gpu`, updated them in `gpuresize()`:

```c
gpu.cellw = win.cw * gpuxscale();
gpu.cellh = win.ch * gpuyscale();
```

and changed `gpucellw()` / `gpucellh()` to return those cached values.

This preserved the actual GPU renderer path, fractional scaling semantics,
glyph/emoji rendering, triangle batches, alpha test, solid no-blend behavior,
accepted clear-color cache, cleared-background skip, and accepted vimnav row
guard. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cache-cell-size/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.860681`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8401 | 1.6725 | 1.8489 | 1.001x | 1.021x | 0.998x |
| repaint | 0.8253 | 1.2339 | 1.8314 | 0.975x | 1.028x | 1.001x |
| scroll_ascii | 0.9831 | 0.9919 | 1.8655 | 0.998x | 1.006x | 1.000x |
| scroll_unicode | 0.9484 | 1.0627 | 1.8289 | 0.997x | 1.018x | 1.000x |
| scroll_emoji | 1.1225 | 0.7854 | 1.8696 | 0.990x | 1.018x | 0.999x |

## Decision

Rejected and reverted.

Caching the scaled cell size marginally helped cursor wall, but repaint and emoji
regressed enough to lower the weighted score. Keep the direct scale calculation in
the accepted renderer.

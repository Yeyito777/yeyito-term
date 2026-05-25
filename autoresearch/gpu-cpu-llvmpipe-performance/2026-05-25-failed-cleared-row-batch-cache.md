# Failed experiment: cache row batches across cleared full redraws

## Hypothesis

When GLX buffer age is unavailable, the accepted GPU path performs a full-frame
clear and calls `tfulldirt()` before drawing. That preserves correctness for
swapped double-buffered llvmpipe drawables, but it means even cursor-only frames
walk every cell and rebuild every row's GPU batches. Caching already-built per-row
GPU batches for safe full-clear frames could reuse vertex data for unchanged rows
while still drawing through the actual GPU renderer.

## Patch summary

The experiment added a `GpuRowCache` array keyed by terminal row. Cache entries
stored a hash of the row glyph data plus copied background/text/color-text/deco
batch vertices. On `gpudrawline()`:

- cache use was limited to full cleared frames (`gpu.clearedframe`) with no global
  reverse mode, no debug mode, no active selection/search, and not the current
  vimnav highlight row,
- a cache hit appended cached vertices to the normal GPU batches,
- a cache miss rendered normally, then copied the appended per-row batch ranges
  into the cache,
- cache entries were invalidated on atlas reset, resize, and color reload/update.

The actual GPU renderer path, dirty/full-redraw behavior, fractional scaling,
glyph/emoji rendering, triangle batches, alpha test, solid no-blend behavior,
accepted clear-color cache, and cleared-background skip were otherwise preserved.
It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cleared-row-batch-cache/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.858747`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8270 | 1.7156 | 1.8512 | 0.994x | 1.024x | 1.000x |
| repaint | 0.8315 | 1.2081 | 1.8335 | 0.992x | 0.992x | 1.002x |
| scroll_ascii | 0.9966 | 0.9769 | 1.8675 | 1.005x | 0.982x | 1.001x |
| scroll_unicode | 0.9260 | 1.0882 | 1.8275 | 0.974x | 1.024x | 1.000x |
| scroll_emoji | 1.1190 | 0.7758 | 1.8701 | 0.977x | 1.003x | 1.001x |

## Decision

Rejected and reverted.

The cache preserved the intended GPU path and behavior constraints, but the extra
hashing/copying/cache management did not pay off. Cursor and repaint wall ratios
both regressed, and the weighted score fell below the accepted clear-color state.
The simple single-pass row renderer remains better for this benchmark state.

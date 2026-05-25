# Failed experiment: skip restoring old cursor cell after cleared full redraw

## Hypothesis

When the GPU path performs a full-frame clear, `xstartdraw()` also calls
`tfulldirt()`, so `drawregion()` redraws the old cursor cell before
`gpudrawcursor()` runs. In that specific frame, the usual overlay draw that
restores the old cursor cell should be redundant. Skipping it when
`gpu.clearedframe` is set might reduce cursor workload overhead without changing
behavior.

## Patch summary

In `render/gpu.c`, changed `gpudrawcursor()` from always restoring the old cursor
cell:

```c
gpudrawcell(og, ox, oy, 1);
```

to doing so only when the frame was not already fully cleared/redrawn:

```c
if (!gpu.clearedframe)
    gpudrawcell(og, ox, oy, 1);
```

This preserved the actual GPU renderer path, cursor drawing, dirty/full-redraw
logic, fractional scaling, glyph/emoji behavior, triangle batches, alpha test,
solid no-blend behavior, accepted clear-color cache, and cleared-background skip.
It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-old-cursor/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.857516`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8297 | 1.6833 | 1.8479 | 0.998x | 1.005x | 0.999x |
| repaint | 0.8246 | 1.2354 | 1.8339 | 0.984x | 1.014x | 1.002x |
| scroll_ascii | 0.9786 | 0.9903 | 1.8657 | 0.987x | 0.996x | 1.001x |
| scroll_unicode | 0.9460 | 1.0714 | 1.8290 | 0.995x | 1.008x | 1.001x |
| scroll_emoji | 1.1290 | 0.7787 | 1.8683 | 0.986x | 1.007x | 1.000x |

## Decision

Rejected and reverted.

The optimization was behavior-preserving for full-redraw frames, but it did not
improve the benchmark. Cursor wall was effectively flat/slightly worse and
repaint/scrolling wall ratios regressed enough to lower the weighted score. The
existing unconditional old-cursor overlay remains better for this benchmark state.

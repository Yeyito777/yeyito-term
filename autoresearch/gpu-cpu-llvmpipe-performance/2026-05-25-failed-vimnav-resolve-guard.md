# Failed experiment: guard vimnav lookup inside GPU color resolution

## Hypothesis

After accepting the `gpudrawline()` guard around `vimnav_curline_y()`, another
inactive-vimnav helper call remained in `gpuresolve()`. Rows that fall out of the
default-background fast path call `gpuresolve()` per cell, and normal benchmark
state has `vimnav.mode == 0`. Guarding the helper there as well might reduce
per-cell work for selected/reverse/debug/non-default cases without changing active
vimnav behavior.

## Patch summary

On top of the accepted row-level vimnav guard, the experiment changed in
`render/gpu.c`:

```c
if (y == vimnav_curline_y() && b == defaultbg)
    b = vimnav_curline_bg;
```

to:

```c
if (vimnav.mode && y == vimnav_curline_y() && b == defaultbg)
    b = vimnav_curline_bg;
```

When vimnav is active, `vimnav_curline_y()` still determines whether the current
line should be highlighted. When inactive, the branch skips the helper.

The actual GPU renderer path, fractional scaling, glyph/emoji rendering, triangle
batches, alpha test, solid no-blend behavior, accepted clear-color cache,
cleared-background skip, and accepted row-level vimnav guard were otherwise
unchanged. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/vimnav-resolve-guard/result.json`

## Result versus accepted row-level vimnav guard

Accepted score: `0.870250`  
Experiment score: `0.859345`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8369 | 1.6394 | 1.8514 | 0.997x | 1.001x | 1.000x |
| repaint | 0.8212 | 1.2423 | 1.8317 | 0.970x | 1.035x | 1.001x |
| scroll_ascii | 0.9810 | 1.0126 | 1.8644 | 0.995x | 1.027x | 1.000x |
| scroll_unicode | 0.9401 | 1.0592 | 1.8280 | 0.988x | 1.014x | 0.999x |
| scroll_emoji | 1.1316 | 0.7723 | 1.8689 | 0.998x | 1.001x | 0.999x |

## Decision

Rejected and reverted.

The helper guard is behavior-preserving, but this path is not hot enough in the
benchmark after the accepted default-background fast path. Repaint regressed badly
and the weighted score fell below the accepted row-level vimnav guard state.

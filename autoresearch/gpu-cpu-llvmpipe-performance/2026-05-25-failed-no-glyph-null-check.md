# Failed experiment: remove redundant gpuglyph null checks

## Hypothesis

`gpuglyph()` always returns a pointer to a cached or newly allocated `GpuGlyph`,
even for empty glyphs. The hot draw paths still check `gg && gg->valid && gg->w > 0
&& gg->h > 0`. Removing the redundant `gg` null check while keeping the accepted
`valid` field/check could simplify the branch without changing behavior.

## Patch summary

In `render/gpu.c`, the experiment changed both draw-path checks from:

```c
if (gg && gg->valid && gg->w > 0 && gg->h > 0)
```

to:

```c
if (gg->valid && gg->w > 0 && gg->h > 0)
```

Actual glyph lookup/loading, empty glyph handling, atlas upload, fractional
scaling, glyph/emoji rendering, batching, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
The benchmark still compared same-source Xft and actual GPU paths under llvmpipe;
it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-glyph-null-check/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.859055`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8346 | 1.6665 | 1.8492 | 0.994x | 1.017x | 0.999x |
| repaint | 0.8190 | 1.2385 | 1.8309 | 0.967x | 1.032x | 1.000x |
| scroll_ascii | 0.9998 | 0.9850 | 1.8657 | 1.014x | 0.999x | 1.000x |
| scroll_unicode | 0.9321 | 1.0715 | 1.8285 | 0.980x | 1.026x | 0.999x |
| scroll_emoji | 1.1255 | 0.7804 | 1.8695 | 0.993x | 1.011x | 0.999x |

## Decision

Rejected and reverted.

The cleanup improved ASCII wall ratio but regressed cursor, repaint, unicode, and
emoji relative to accepted. Keep the accepted glyph check expression.

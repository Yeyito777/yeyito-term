# Failed experiment: remove redundant GPU glyph valid flag

## Hypothesis

`gpuglyph()` always returns a `GpuGlyph *` for cached/loaded glyph entries and sets
`g->valid = 1` before returning, including empty glyphs. The hot draw paths still
check `gg && gg->valid && gg->w > 0 && gg->h > 0`. Removing the redundant `valid`
field and the always-true checks could shrink glyph metadata and simplify per-cell
rendering without changing behavior; empty glyphs are still filtered by width and
height.

## Patch summary

In `render/gpu.c`, the experiment removed `int valid` from `GpuGlyph`, deleted the
`g->valid = 1` assignment in `gpuglyph()`, and changed the draw-path checks from:

```c
if (gg && gg->valid && gg->w > 0 && gg->h > 0)
```

to:

```c
if (gg->w > 0 && gg->h > 0)
```

Actual glyph lookup/loading, atlas allocation/upload, empty glyph handling,
fractional scaling, glyph/emoji rendering, batching, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
The benchmark still compared same-source Xft and actual GPU paths under llvmpipe;
it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-glyph-valid/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.869733`  
Relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8463 | 1.6583 | 1.8529 | 1.008x | 1.012x | 1.001x |
| repaint | 0.8077 | 1.2034 | 1.8308 | 0.954x | 1.003x | 1.000x |
| scroll_ascii | 0.9975 | 0.9731 | 1.8624 | 1.012x | 0.987x | 0.999x |
| scroll_unicode | 0.9302 | 0.8446 | 1.8315 | 0.978x | 0.809x | 1.001x |
| scroll_emoji | 1.1415 | 0.7642 | 1.8700 | 1.007x | 0.990x | 0.999x |

## Decision

Rejected and reverted.

The cleanup was close to accepted and improved cursor, ASCII, and emoji wall ratios,
but repaint and unicode wall ratios regressed sharply enough that the weighted
score stayed below accepted. Keep the accepted `valid` field/check layout.

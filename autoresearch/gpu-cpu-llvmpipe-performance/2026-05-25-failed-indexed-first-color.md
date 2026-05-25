# Failed experiment: prefer indexed-color branch in gpucolor

## Hypothesis

Most benchmark colors are palette-indexed rather than truecolor. Reordering
`gpucolor()` to handle the indexed-color path first could improve branch layout in
hot text/background color resolution without changing color semantics.

## Patch summary

In `render/gpu.c`, the experiment changed `gpucolor()` from checking
`IS_TRUECOL(c)` first to checking `!IS_TRUECOL(c)` first and returning after the
palette lookup. Truecolor conversion remained identical in the fallback path.

Actual color values, truecolor behavior, batching, glyph/emoji rendering,
fractional scaling, accepted clear-color cache, cleared-background skip, and
accepted vimnav row guard were otherwise unchanged. The benchmark still compared
same-source Xft and actual GPU paths under llvmpipe; it did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/indexed-first-color/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.854688`  
Relative score: `0.982x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8260 | 1.7020 | 1.8481 | 0.984x | 1.039x | 0.998x |
| repaint | 0.8252 | 1.2287 | 1.8299 | 0.975x | 1.024x | 1.000x |
| scroll_ascii | 0.9822 | 0.9973 | 1.8624 | 0.997x | 1.012x | 0.998x |
| scroll_unicode | 0.9380 | 1.0879 | 1.8306 | 0.986x | 1.042x | 1.001x |
| scroll_emoji | 1.1226 | 0.7925 | 1.8687 | 0.990x | 1.027x | 0.999x |

## Decision

Rejected and reverted.

The branch reordering regressed every workload's wall ratio relative to accepted
and significantly lowered the weighted score. Keep the accepted truecolor-first
layout in `gpucolor()`.

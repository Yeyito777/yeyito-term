# Failed experiment: cache GPU cell positions

## Hypothesis

The GPU renderer computes cell pixel coordinates with fractional-scaling aware
rounding helpers (`gpucellx()`, `gpucelly()`, `gpucellright()`, `gpurowbottom()`)
for every cell. Precomputing the rounded x/y cell-edge positions once per frame
could reduce repeated floating-point scaling/rounding in the hot drawing path
while preserving fractional scaling exactly.

## Patch summary

The experiment added `cellx` / `celly` arrays to `Gpu`, filled them in
`gpuresize()` from the same formulas used by the existing helpers, and changed
`gpucellx()` / `gpucelly()` to read cached positions when the requested coordinate
was in range. It also freed the arrays in `gpudestroy()`.

The fallback helper formulas were kept for out-of-range values. The actual GPU
renderer path, fractional-scaling behavior, glyph/emoji rendering, accepted
clear-color cache, cleared-background skip, triangle batches, alpha test, and
solid no-blend behavior were otherwise unchanged. This did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cell-position-cache/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.855284`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8362 | 1.6432 | 1.8478 | 1.006x | 0.981x | 0.999x |
| repaint | 0.8009 | 1.2778 | 1.8322 | 0.956x | 1.049x | 1.001x |
| scroll_ascii | 0.9904 | 0.9744 | 1.8635 | 0.998x | 0.980x | 0.999x |
| scroll_unicode | 0.9332 | 1.0724 | 1.8311 | 0.982x | 1.009x | 1.002x |
| scroll_emoji | 1.1376 | 0.7702 | 1.8700 | 0.993x | 0.996x | 1.001x |

## Decision

Rejected and reverted.

The cache helped cursor wall slightly but badly regressed repaint and total score.
The per-frame cache fill and extra indirection outweighed saved scaling/rounding
work. The original helper-based coordinate calculation remains better and simpler
in this benchmark state.

# Failed experiment: inline row baseline calculation

## Hypothesis

`gpudrawline()` computes both `gpurowbottom(y)` and `gpubaseline(y)`. The baseline
helper recomputes the row top and bottom internally. Reusing the already-computed
row top/bottom and inlining the baseline formula could reduce per-row scaling and
rounding work. `gpudrawcell()` can similarly compute the baseline from its
already-known cell top/height.

## Patch summary

In `render/gpu.c`:

- changed `gpudrawline()` to compute `bottom = gpurowbottom(y)`, `rowh = bottom - basey`,
  and then compute `baseline` directly from `basey`, `rowh`, `gpu.ascent`, and
  `gpu.descent`,
- changed `gpudrawcell()` to compute `baseline` directly from `celly` and `cellh`.

The formula was identical to `gpubaseline()`. The actual GPU renderer path,
accepted alpha test, solid no-blend path, triangle batches, fractional scaling,
glyph atlas rendering, and color emoji behavior were otherwise unchanged. This
did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/inline-row-baseline/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.748247`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6440 | 2.5272 | 1.8495 | 0.987x | 1.046x | 0.999x |
| repaint | 0.7518 | 1.4326 | 1.8309 | 1.003x | 0.993x | 1.000x |
| scroll_ascii | 0.9194 | 1.1039 | 1.8637 | 0.998x | 1.016x | 1.000x |
| scroll_unicode | 0.8677 | 1.1935 | 1.8259 | 0.957x | 1.064x | 0.999x |
| scroll_emoji | 1.0875 | 0.8281 | 1.8711 | 0.999x | 1.007x | 1.000x |

## Decision

Rejected and reverted.

The inline formula slightly helped repaint, but it regressed the weighted score
and cursor updates substantially. The helper-based form is clearer and performs
better overall under the accepted benchmark state.

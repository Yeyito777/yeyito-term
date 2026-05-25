# Failed experiment: branch prediction hints in GPU row/cell renderer

## Hypothesis

The GPU renderer has a few very hot common-case branches: most cells are not
`ATTR_WDUMMY`, most rendered cells are not spaces in scrolling/repaint workloads,
and most cells use the accepted default-background fast path. Adding explicit
`likely()` / `unlikely()` hints might help the compiler arrange branch code for
better instruction-cache/branch-prediction behavior under llvmpipe CPU rendering.

## Patch summary

In `render/gpu.c`, the experiment added local `likely` / `unlikely` macros using
`__builtin_expect()` and applied them to:

- `ATTR_WDUMMY` checks in `gpudrawline()` and `gpudrawcell()`,
- the accepted default-background fast-path condition in `gpudrawline()`,
- non-space glyph checks in `gpudrawline()` and `gpudrawcell()`.

No rendering behavior, GL state, batching, glyph/emoji rendering, fractional
scaling, accepted clear-color cache, cleared-background skip, or accepted vimnav
row guard was changed. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/gpu-branch-hints/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.857709`  
Relative score: `0.986x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8417 | 1.6382 | 1.8477 | 1.003x | 1.000x | 0.998x |
| repaint | 0.8170 | 1.2528 | 1.8301 | 0.965x | 1.044x | 1.000x |
| scroll_ascii | 0.9747 | 1.0302 | 1.8649 | 0.989x | 1.045x | 1.000x |
| scroll_unicode | 0.9459 | 1.0693 | 1.8294 | 0.994x | 1.024x | 1.000x |
| scroll_emoji | 1.1285 | 0.7759 | 1.8698 | 0.995x | 1.006x | 0.999x |

## Decision

Rejected and reverted.

The hints did not improve the weighted score and regressed the high-priority
repaint workload materially. The current compiler-generated branch layout is
better than the annotated variant.

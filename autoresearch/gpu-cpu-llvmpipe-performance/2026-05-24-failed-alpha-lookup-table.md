# Failed experiment: glyph alpha lookup table

## Hypothesis

Monochrome/gray glyph uploads apply `gpualpha()` to every gray coverage byte while
building the tight glyph atlas upload buffer. Each benchmark workload starts fresh
processes and rebuilds glyph caches, so replacing the per-pixel branch/arithmetic
with a 256-entry lookup table might reduce glyph warmup CPU without changing text
appearance.

## Patch summary

In `render/gpu.c`, the experiment added:

```c
static unsigned char gpualphatab[256];
```

initialized in `gpuinit()` with the exact accepted transfer function:

```c
gpualphatab[i] = i < 16 ? 0 : MIN(255, (i - 16) * 280 / 239);
```

and changed `gpualpha(a)` to return `gpualphatab[a]`. This preserved the exact
coverage values, actual GPU renderer path, glyph/emoji behavior, fractional
scaling, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, and cleared-background skip. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/alpha-lookup-table/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/alpha-lookup-table-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.859232`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8381 | 1.6435 | 1.8485 | 1.008x | 0.981x | 0.999x |
| repaint | 0.8269 | 1.2285 | 1.8315 | 0.987x | 1.008x | 1.001x |
| scroll_ascii | 0.9756 | 0.9957 | 1.8656 | 0.983x | 1.001x | 1.000x |
| scroll_unicode | 0.9273 | 1.0745 | 1.8287 | 0.976x | 1.011x | 1.001x |
| scroll_emoji | 1.1279 | 0.7899 | 1.8688 | 0.985x | 1.021x | 1.000x |

## Decision

Rejected and reverted.

The first run was barely positive (`0.865774`) with a repaint-wall improvement,
but validation failed to reproduce it. The validated score fell below accepted and
repaint/scrolling wall ratios regressed. Glyph alpha conversion is not a reliable
bottleneck in the current llvmpipe state.

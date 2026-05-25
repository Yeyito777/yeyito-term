# Failed experiment: build with -funroll-loops

## Hypothesis

The accepted build already uses `-O3 -march=native -flto`, but the renderer and
benchmark hot paths include many small loops: glyph alpha conversion, color emoji
crop/copy loops, per-row cell processing, and Xft comparison paths. Adding
`-funroll-loops` might let the compiler reduce loop overhead in the CPU-backed GPU
renderer enough to improve the llvmpipe score.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -funroll-loops
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/unroll-loops/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/unroll-loops-validate/result.json`

## Validation result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment validation score: `0.860021`  
Relative score: `0.988x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8281 | 1.6353 | 1.8490 | 0.987x | 0.998x | 0.998x |
| repaint | 0.8452 | 1.2440 | 1.8301 | 0.998x | 1.036x | 1.000x |
| scroll_ascii | 0.9698 | 1.0104 | 1.8648 | 0.984x | 1.025x | 1.000x |
| scroll_unicode | 0.9402 | 1.0594 | 1.8300 | 0.988x | 1.015x | 1.000x |
| scroll_emoji | 1.1198 | 0.7810 | 1.8667 | 0.988x | 1.012x | 0.998x |

## Decision

Rejected and reverted.

The first 7-iteration run was slightly positive (`0.871250`, about `1.001x`
accepted), but the 9-iteration validation did not reproduce the gain and regressed
all wall workloads versus accepted. Keep the accepted build flags.

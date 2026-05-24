# Failed experiment: 1024-vertex batch growth after cleared-background skip

## Hypothesis

The accepted cleared-background skip removes many solid background vertices. The
accepted 2048-vertex batch growth slack might now be larger than necessary;
reducing it to 1024 could reduce memory churn/RSS without causing too many
reallocations in the smaller post-background-skip batches.

## Patch summary

In `render/gpu.c`, changed `gpubatchalloc()` from:

```c
b->cap = MAX(b->cap * 2, b->len + n + 2048);
```

to:

```c
b->cap = MAX(b->cap * 2, b->len + n + 1024);
```

The actual GPU renderer path, accepted cleared-background skip, alpha test, solid
no-blend behavior, triangle batches, fractional scaling, glyph atlas rendering,
and color emoji path were otherwise unchanged. This did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/bgskip-batch-growth-1024/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.854619`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8390 | 1.6686 | 1.8516 | 0.999x | 1.008x | 1.001x |
| repaint | 0.8126 | 1.2689 | 1.8316 | 0.978x | 1.031x | 1.000x |
| scroll_ascii | 0.9591 | 1.0144 | 1.8666 | 0.980x | 1.018x | 1.000x |
| scroll_unicode | 0.9371 | 1.0734 | 1.8286 | 1.001x | 1.002x | 1.000x |
| scroll_emoji | 1.1508 | 0.7650 | 1.8684 | 1.006x | 1.003x | 0.999x |

## Decision

Rejected and reverted.

The smaller growth slack regressed weighted score, repaint, and ASCII scrolling.
The accepted 2048-vertex slack remains better even after cleared-background
skipping reduced solid batch size.

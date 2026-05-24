# Failed experiment: restore 4096-vertex batch growth for triangles

## Hypothesis

After switching batches from quads to triangles, each quad emits six vertices
instead of four. The accepted 2048-vertex growth slack might now be too small,
causing extra reallocations. Restoring 4096-vertex slack could reduce allocation
churn while keeping the accepted triangle draw path.

## Patch summary

Changed `gpubatchalloc()` in `render/gpu.c` from the accepted triangle-batch
state:

```c
b->cap = MAX(b->cap * 2, b->len + n + 2048);
```

to:

```c
b->cap = MAX(b->cap * 2, b->len + n + 4096);
```

This remained on the actual GPU renderer path and did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/triangle-batch-growth-4096/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.705568`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5814 | 2.8888 | 1.8528 | 0.988x | 1.013x | 1.001x |
| repaint | 0.6982 | 1.5625 | 1.8308 | 0.989x | 1.018x | 1.001x |
| scroll_ascii | 0.9032 | 1.1280 | 1.8647 | 0.994x | 1.011x | 1.000x |
| scroll_unicode | 0.8625 | 1.2099 | 1.8276 | 1.013x | 0.973x | 1.001x |
| scroll_emoji | 1.0466 | 0.8690 | 1.8709 | 0.977x | 1.025x | 1.001x |

## Decision

Rejected and reverted.

The larger growth slack regressed weighted score and both high-priority
cursor/repaint wall ratios. The accepted 2048-vertex slack remains better even
with triangle-expanded batches.

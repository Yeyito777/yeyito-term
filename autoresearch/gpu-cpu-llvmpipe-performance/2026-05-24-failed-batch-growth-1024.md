# Failed experiment: 1024-vertex GPU batch growth slack

## Hypothesis

After accepting 2048-vertex batch growth slack, reducing the slack further to
1024 vertices might reduce client-side batch over-allocation under llvmpipe.

## Patch summary

Changed `gpubatchalloc()` in `render/gpu.c` from the accepted `+ 2048` slack to
`+ 1024`:

```c
b->cap = MAX(b->cap * 2, b->len + n + 1024);
```

The experiment preserved the GPU renderer path and still used the same batched
`glDrawArrays` model; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/batch-growth-1024/result.json`

## Result versus accepted 2048-slack state

Accepted score: `0.712101`  
Experiment score: `0.709549`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5798 | 2.8896 | 1.8497 | 0.994x | 1.036x | 0.999x |
| repaint | 0.7107 | 1.5534 | 1.8271 | 1.005x | 0.991x | 0.999x |
| scroll_ascii | 0.8973 | 1.1332 | 1.8581 | 0.977x | 1.024x | 0.999x |
| scroll_unicode | 0.8647 | 1.2163 | 1.8293 | 1.009x | 0.992x | 1.000x |
| scroll_emoji | 1.0694 | 0.8529 | 1.8672 | 1.003x | 0.989x | 0.999x |

## Decision

Rejected and reverted.

The smaller growth slack regressed weighted score, hurt cursor, and did not
produce meaningful RSS benefit. Keep the accepted 2048-vertex slack.

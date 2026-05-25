# Failed experiment: add -fno-trapping-math

## Hypothesis

The GPU renderer's hot path does substantial floating-point coordinate and color
work before feeding llvmpipe. Since the renderer does not depend on floating-point
traps, adding `-fno-trapping-math` might let GCC simplify the math enough to help
llvmpipe throughput without changing the actual GPU path.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-trapping-math
```

No renderer fallback, `gpudraw` bypass, llvmpipe detection, batching semantic, text,
emoji, fractional scaling, clear-color cache, cleared-background skip, or accepted
vimnav row guard behavior was changed.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-trapping-math/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.860462`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8276 | 1.6699 | 1.8529 | 0.986x | 1.019x | 1.001x |
| repaint | 0.8429 | 1.2034 | 1.8316 | 0.996x | 1.003x | 1.001x |
| scroll_ascii | 0.9774 | 1.0046 | 1.8636 | 0.992x | 1.019x | 0.999x |
| scroll_unicode | 0.9309 | 1.0712 | 1.8275 | 0.978x | 1.026x | 0.999x |
| scroll_emoji | 1.1217 | 0.7834 | 1.8693 | 0.989x | 1.015x | 0.999x |

## Decision

Rejected and reverted.

The flag improved some CPU ratios, but every wall-speedup ratio was below the
accepted frontier, causing the weighted score to regress. Keep the accepted build
flags.

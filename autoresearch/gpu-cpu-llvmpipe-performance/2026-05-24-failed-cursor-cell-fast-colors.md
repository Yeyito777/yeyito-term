# Failed experiment: fast default color path for GPU cursor cells

## Hypothesis

`gpudrawcell()` is used for cursor overlay drawing and currently calls the full
`gpuresolve()` color/mode path for every old/new cursor cell. The line renderer
already has a fast path for common default-background cells. Reusing the same
predicate in `gpudrawcell()` might reduce cursor-update CPU cost under llvmpipe
without changing the GPU renderer path.

## Patch summary

The experiment added a common-case branch in `gpudrawcell()`:

- If the cell had default background and no selected/search/reverse/blink/faint
  or other special modes, resolve foreground/background directly with
  `gpucolor()`.
- Otherwise keep the existing `gpuresolve()` path.

This preserved the GPU renderer and fractional scaling behavior; it did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 7 --warmups 2 \
  --name st-llvmpipe-cellfast \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/cursor-cell-fast-colors
```

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cursor-cell-fast-colors/result.json`

## Result versus accepted lazy-color-atlas state

Accepted score: `0.709839`  
Experiment score: `0.707890`  
Relative score: `0.997x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5841 | 2.8566 | 1.8701 | 1.005x | 1.007x |
| repaint | 0.7132 | 1.5294 | 1.8456 | 1.010x | 0.993x |
| scroll_ascii | 0.8983 | 1.1706 | 1.8762 | 0.997x | 1.044x |
| scroll_unicode | 0.8596 | 1.2210 | 1.8443 | 0.997x | 1.016x |
| scroll_emoji | 1.0408 | 0.8703 | 1.9532 | 0.978x | 1.015x |

## Decision

Rejected and reverted.

The change slightly improved cursor and repaint wall ratios, but the weighted
score regressed below the accepted lazy-color-atlas state. It also made cursor
CPU ratio slightly worse and shifted cost/noise into other workloads. The
acceptance rule is weighted score improvement without material cursor/repaint
regression, so this does not qualify.

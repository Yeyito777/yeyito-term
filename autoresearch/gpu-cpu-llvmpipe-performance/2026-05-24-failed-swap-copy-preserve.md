# Failed experiment: trust GLX swap-copy preservation

## Hypothesis

When GLX buffer age is unavailable, the accepted renderer conservatively clears
and redraws the full frame after each swap because the back buffer contents are
undefined. Some GLX visuals report `GLX_SWAP_METHOD_OML == GLX_SWAP_COPY_OML`,
which means the drawable preserves contents by copying on swap. If the visual
advertises copy-preserving swaps, the renderer could skip the full-clear/full-dirt
fallback and draw only dirty rows while remaining correct.

## Patch summary

In `render/gpu.c`, the experiment added a `gpu.swappreserve` flag and set it in
`gpuinit()` when:

```c
glXGetConfig(xw.dpy, vi, GLX_SWAP_METHOD_OML, &swapmethod) == 0 &&
swapmethod == GLX_SWAP_COPY_OML
```

In `x.c`, the full-clear fallback became conditional on `!gpu.swappreserve`:

```c
if (gpu.needclear ||
    (gpu.doublebuf && !gpu.swappreserve &&
     (!gpu.bufferage || gpu.backage == 0 || gpu.backage > GPU_DAMAGE_HISTORY)))
    ...
```

The experiment preserved the actual GPU renderer path, swaps, dirty-row drawing,
fractional scaling, GPU text/emoji behavior, triangle batches, alpha test, solid
no-blend behavior, accepted clear-color cache, and cleared-background skip. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/swap-copy-preserve/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.857731`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8397 | 1.6415 | 1.8511 | 1.010x | 0.980x | 1.000x |
| repaint | 0.8125 | 1.2558 | 1.8315 | 0.970x | 1.031x | 1.001x |
| scroll_ascii | 0.9884 | 0.9750 | 1.8683 | 0.996x | 0.980x | 1.002x |
| scroll_unicode | 0.9259 | 1.0729 | 1.8298 | 0.974x | 1.010x | 1.001x |
| scroll_emoji | 1.1228 | 0.7732 | 1.8735 | 0.980x | 0.999x | 1.003x |

## Decision

Rejected and reverted.

The swap-copy check did not improve this llvmpipe/Xephyr benchmark state. Repaint
and unicode wall ratios regressed materially, and the weighted score fell. The
accepted conservative full-clear path remains better and safer here.

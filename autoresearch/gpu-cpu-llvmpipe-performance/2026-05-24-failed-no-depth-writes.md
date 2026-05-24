# Failed experiment: disable depth writes

## Hypothesis

The GPU renderer draws a strictly 2D terminal scene and does not use depth. Even
if the GLX visual has no useful depth buffer, explicitly disabling depth testing
and depth writes might avoid llvmpipe depth-state work during clears and triangle
rasterization.

## Patch summary

In `render/gpu.c`, the experiment added to `gpuinit()`:

```c
glDisable(GL_DEPTH_TEST);
glDepthMask(GL_FALSE);
```

The actual GPU renderer path, triangle batches, glyph/emoji atlases, alpha test,
solid no-blend behavior, accepted clear-color cache, and cleared-background skip
were otherwise unchanged. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-depth-writes/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-depth-writes-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.862472`  
Relative score: `0.997x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8204 | 1.6739 | 1.8500 | 0.987x | 0.999x | 1.000x |
| repaint | 0.8459 | 1.2051 | 1.8314 | 1.009x | 0.989x | 1.001x |
| scroll_ascii | 0.9877 | 0.9977 | 1.8641 | 0.996x | 1.003x | 1.000x |
| scroll_unicode | 0.9303 | 1.0715 | 1.8303 | 0.979x | 1.008x | 1.001x |
| scroll_emoji | 1.1402 | 0.7653 | 1.8671 | 0.995x | 0.989x | 0.999x |

## Decision

Rejected and reverted.

The initial run was very slightly positive (`0.865289`), but validation failed to
reproduce the gain. Repaint wall improved, but cursor and scrolling wall ratios
regressed enough to lower the weighted score. The explicit depth-state changes do
not help reliably in this llvmpipe benchmark state.

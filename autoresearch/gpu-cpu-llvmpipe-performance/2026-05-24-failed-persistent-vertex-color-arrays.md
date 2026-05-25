# Failed experiment: persistent vertex/color client arrays

## Hypothesis

Every GPU batch uses vertex and color arrays. The renderer currently enables
`GL_VERTEX_ARRAY` and `GL_COLOR_ARRAY` for every non-empty batch, then disables
them at frame finish. Enabling those client states once during GPU initialization
and leaving them enabled across frames might reduce fixed-function GL state
traffic under llvmpipe.

## Patch summary

In `render/gpu.c`, the experiment enabled vertex/color client arrays once in
`gpuinit()`:

```c
glEnableClientState(GL_VERTEX_ARRAY);
glEnableClientState(GL_COLOR_ARRAY);
```

and removed the per-batch enables in `gpudrawbatch()`. In `x.c`, it also removed
the frame-end disables for `GL_VERTEX_ARRAY` and `GL_COLOR_ARRAY`; the existing
texture-coordinate enable/disable behavior was left unchanged.

The actual GPU renderer path, triangle batches, texture-coordinate handling,
glyph/emoji behavior, fractional scaling, alpha test, solid no-blend behavior,
accepted clear-color cache, and cleared-background skip were otherwise preserved.
It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/persistent-vertex-color-arrays/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.860251`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8321 | 1.6394 | 1.8496 | 1.001x | 0.979x | 1.000x |
| repaint | 0.8247 | 1.2381 | 1.8314 | 0.984x | 1.016x | 1.001x |
| scroll_ascii | 0.9788 | 0.9810 | 1.8687 | 0.987x | 0.986x | 1.002x |
| scroll_unicode | 0.9479 | 1.0632 | 1.8280 | 0.997x | 1.000x | 1.000x |
| scroll_emoji | 1.1310 | 0.7778 | 1.8676 | 0.987x | 1.005x | 1.000x |

## Decision

Rejected and reverted.

Leaving vertex/color arrays enabled helped neither the weighted score nor the
high-priority repaint workload. Repaint and most scrolling wall ratios regressed,
so the existing per-batch client-state enables remain better for this benchmark
state.

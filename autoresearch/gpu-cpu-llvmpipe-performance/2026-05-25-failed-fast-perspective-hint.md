# Failed experiment: GL_FASTEST perspective correction hint

## Hypothesis

The renderer uses 2D orthographic quads and does not need perspective-correct
texture interpolation. On fixed-function OpenGL under llvmpipe, requesting
`GL_FASTEST` for `GL_PERSPECTIVE_CORRECTION_HINT` might allow cheaper rasterization
or setup while preserving visible behavior for affine 2D quads.

## Patch summary

In `render/gpu.c`, the experiment added during GPU initialization:

```c
glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
```

No batching, texture upload, glyph rendering, fractional scaling, clear behavior,
or blend/alpha state was changed. The actual GPU renderer path, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard were
preserved. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/fast-perspective-hint/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.859191`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8423 | 1.6337 | 1.8508 | 1.003x | 0.997x | 0.999x |
| repaint | 0.8154 | 1.2449 | 1.8300 | 0.963x | 1.037x | 1.000x |
| scroll_ascii | 0.9779 | 0.9920 | 1.8624 | 0.992x | 1.006x | 0.998x |
| scroll_unicode | 0.9501 | 1.0622 | 1.8289 | 0.999x | 1.017x | 1.000x |
| scroll_emoji | 1.1122 | 0.7733 | 1.8674 | 0.981x | 1.002x | 0.998x |

## Decision

Rejected and reverted.

The hint did not help llvmpipe and regressed repaint substantially. Keep the
accepted GL state unchanged.

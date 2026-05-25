# Failed experiment: disable GL dithering

## Hypothesis

OpenGL enables dithering by default. On an RGBA visual under llvmpipe, dithering
should not be useful for the terminal renderer and might add avoidable software
rasterization work. Explicitly disabling `GL_DITHER` during GPU initialization
could reduce fragment overhead without changing the renderer path.

## Patch summary

In `render/gpu.c`, the experiment added during `gpuinit()`:

```c
glDisable(GL_DITHER);
```

No drawing paths, batches, glyph uploads, clear logic, blend/alpha-test behavior,
or atlas behavior were changed.

This preserved the actual GPU renderer path, fractional scaling, glyph/emoji
rendering, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, and cleared-background skip. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-dither/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.863938`  
Relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8454 | 1.6497 | 1.8509 | 1.017x | 0.985x | 1.000x |
| repaint | 0.8384 | 1.2174 | 1.8309 | 1.000x | 0.999x | 1.000x |
| scroll_ascii | 0.9557 | 1.0209 | 1.8655 | 0.963x | 1.026x | 1.000x |
| scroll_unicode | 0.9452 | 1.0655 | 1.8283 | 0.994x | 1.003x | 1.000x |
| scroll_emoji | 1.1431 | 0.7740 | 1.8676 | 0.998x | 1.000x | 1.000x |

## Decision

Rejected and reverted.

Disabling dithering improved cursor wall time, but ASCII scrolling regressed enough
to pull the weighted score slightly below the accepted clear-color cache state.
The default dither state remains preferable for this benchmark mix.

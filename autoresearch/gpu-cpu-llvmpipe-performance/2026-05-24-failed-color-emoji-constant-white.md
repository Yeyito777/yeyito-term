# Failed experiment: constant white color for color emoji batches

## Hypothesis

Color emoji batches always multiply the color texture by white. Disabling the
per-vertex color array for color emoji batches and using a constant white
`glColor4f(1, 1, 1, 1)` might reduce llvmpipe client-array fetch work for the
emoji texture path.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` was changed so `textured == 2` color-emoji
batches:

- disabled `GL_COLOR_ARRAY`,
- used `glColor4f(1.0f, 1.0f, 1.0f, 1.0f)`,
- continued to use the same color atlas, texture coordinate array, triangle
  batches, and color-emoji blend function.

Normal text and solid batches kept their existing color-array paths. This stayed
on the actual GPU renderer path and did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/color-emoji-constant-white/result.json`

## Result versus accepted disable-solid-blend state

Accepted score: `0.750395`  
Experiment score: `0.742564`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6536 | 2.4294 | 1.8505 | 1.013x | 0.986x | 1.000x |
| repaint | 0.7161 | 1.4607 | 1.8315 | 0.962x | 1.017x | 1.002x |
| scroll_ascii | 0.9269 | 1.0786 | 1.8640 | 1.004x | 0.998x | 0.999x |
| scroll_unicode | 0.8774 | 1.1828 | 1.8263 | 0.992x | 1.014x | 1.000x |
| scroll_emoji | 1.0536 | 0.8560 | 1.8698 | 0.971x | 1.035x | 0.999x |

## Decision

Rejected and reverted.

The change hurt the weighted score and materially regressed repaint and emoji
wall ratios. The extra client-state switch plus constant-color path was worse
than keeping the simple per-vertex white color array for color emoji batches.

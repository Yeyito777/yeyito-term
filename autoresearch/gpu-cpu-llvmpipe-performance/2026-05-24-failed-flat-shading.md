# Failed experiment: flat shading

## Hypothesis

Every GPU batch currently assigns the same color to all vertices of a rectangle
or glyph quad. OpenGL's default smooth shading may still make llvmpipe carry
color interpolation state/work across triangles. Switching fixed-function shading
to `GL_FLAT` could avoid unnecessary color interpolation while preserving the
same final color because each primitive is uniformly colored.

## Patch summary

In `render/gpu.c`, the experiment added to `gpuinit()`:

```c
glShadeModel(GL_FLAT);
```

right after pixel-store setup. The rest of the renderer was unchanged: actual GPU
path, triangle batches, texture coordinates, glyph/emoji atlases, alpha test,
solid no-blend behavior, accepted clear-color cache, and cleared-background skip
were preserved. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/flat-shading/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/flat-shading-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.864106`  
Relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8387 | 1.6496 | 1.8499 | 1.009x | 0.985x | 1.000x |
| repaint | 0.8293 | 1.2173 | 1.8330 | 0.990x | 0.999x | 1.002x |
| scroll_ascii | 0.9889 | 0.9792 | 1.8655 | 0.997x | 0.984x | 1.000x |
| scroll_unicode | 0.9371 | 1.0663 | 1.8285 | 0.986x | 1.003x | 1.000x |
| scroll_emoji | 1.1344 | 0.7776 | 1.8698 | 0.990x | 1.005x | 1.001x |

## Decision

Rejected and reverted.

The initial run looked slightly positive (`0.867988`), but validation failed to
reproduce the gain. The validated total score fell below the accepted clear-color
state cache, and repaint plus scrolling wall ratios regressed despite a small
cursor wall improvement. Keeping the default smooth-shading state is better for
this benchmark state.

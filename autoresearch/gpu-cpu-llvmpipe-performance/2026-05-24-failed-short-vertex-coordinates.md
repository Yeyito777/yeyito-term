# Failed experiment: short integer vertex coordinates

## Hypothesis

All terminal quad positions are integer pixel coordinates after fractional-scaling
rounding. Storing vertex `x`/`y` as `GLshort` instead of `GLfloat` could shrink the
interleaved vertex structure and reduce client-array bandwidth for llvmpipe while
preserving exact pixel positions.

## Patch summary

In `render/gpu.c`, changed `GpuVertex` from:

```c
GLfloat x, y;
```

to:

```c
GLshort x, y;
```

and changed the vertex pointer setup from:

```c
glVertexPointer(2, GL_FLOAT, sizeof(GpuVertex), voff);
```

to:

```c
glVertexPointer(2, GL_SHORT, sizeof(GpuVertex), voff);
```

Texture coordinates and colors remained floats. The actual GPU renderer path,
fractional-scaling rounding, glyph/emoji behavior, triangle batches, alpha test,
solid no-blend behavior, accepted clear-color cache, and cleared-background skip
were otherwise unchanged. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/short-vertex-xy/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/short-vertex-xy-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.856880`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8361 | 1.6453 | 1.8470 | 1.005x | 0.982x | 0.998x |
| repaint | 0.8188 | 1.2395 | 1.8286 | 0.977x | 1.018x | 0.999x |
| scroll_ascii | 0.9764 | 1.0267 | 1.8645 | 0.984x | 1.032x | 1.000x |
| scroll_unicode | 0.9388 | 1.0788 | 1.8278 | 0.988x | 1.015x | 1.000x |
| scroll_emoji | 1.1295 | 0.7746 | 1.8705 | 0.986x | 1.001x | 1.001x |

## Decision

Rejected and reverted.

The initial run was barely positive (`0.865726`), but validation failed to
reproduce it. Repaint and scrolling wall ratios regressed materially, so the
accepted float coordinate layout remains better under llvmpipe.

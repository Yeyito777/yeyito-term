# Failed experiment: float batch coordinate helpers

## Hypothesis

`GpuVertex` stores positions and texture coordinates as `GLfloat`, but the batch
helper functions accept `double` coordinates and texture coordinates. Changing
`gpubatchquad()`, `gpubatchrect()`, and `gpubatchglyph()` to accept `float` and
computing glyph texture coordinates as `float` might reduce conversion overhead
in the hot batching path while preserving enough precision for window-space
integer positions and 1024px atlases.

## Patch summary

In `render/gpu.c`:

- changed batch helper coordinate parameters from `double` to `float`,
- made their color parameter `const float c[3]`,
- computed glyph texture coordinates with `0.5f` constants into `float` locals.

The actual GPU renderer path, accepted alpha test, solid no-blend path, triangle
batches, fractional scaling, glyph atlas rendering, and color emoji behavior were
otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/float-batch-coordinates/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.745447`  
Relative score: `0.986x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6492 | 2.4531 | 1.8519 | 0.995x | 1.015x | 1.000x |
| repaint | 0.7391 | 1.5401 | 1.8298 | 0.986x | 1.067x | 1.000x |
| scroll_ascii | 0.9233 | 1.0813 | 1.8634 | 1.002x | 0.995x | 1.000x |
| scroll_unicode | 0.8699 | 1.1670 | 1.8262 | 0.959x | 1.041x | 0.999x |
| scroll_emoji | 1.0808 | 0.8302 | 1.8687 | 0.993x | 1.010x | 0.999x |

## Decision

Rejected and reverted.

The float helper signatures regressed the weighted score and the high-priority
cursor/repaint workloads, especially repaint CPU. The compiler evidently handles
the original double-to-float stores well enough, and the original signatures keep
callers simple.

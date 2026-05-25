# Failed experiment: specialize solid rectangle batching

## Hypothesis

Solid background and decoration rectangles do not need texture coordinates, but
`gpubatchrect()` calls the generic quad helper with zero texture coordinates. A
specialized solid-rectangle batch writer could avoid passing/storing texture
coordinate setup for solid batches and reduce background/deco batching overhead.

## Patch summary

In `render/gpu.c`, the experiment replaced `gpubatchrect()`'s call to
`gpubatchquad(..., 0, 0, 0, 0, c)` with a specialized six-vertex writer that filled
only position and color fields for the two triangles.

This preserved the actual GPU renderer path, triangle batching, solid no-blend
behavior, glyph/emoji rendering, fractional scaling, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard. It did not fallback to Xft
or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-rect-specialized/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.854607`  
Relative score: `0.982x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8388 | 1.6405 | 1.8508 | 0.999x | 1.002x | 0.999x |
| repaint | 0.8188 | 1.2502 | 1.8312 | 0.967x | 1.042x | 1.001x |
| scroll_ascii | 0.9554 | 1.0156 | 1.8625 | 0.969x | 1.030x | 0.999x |
| scroll_unicode | 0.9380 | 1.0695 | 1.8290 | 0.986x | 1.024x | 1.000x |
| scroll_emoji | 1.1270 | 0.7917 | 1.8685 | 0.994x | 1.026x | 0.998x |

## Decision

Rejected and reverted.

The specialized writer made solid batching slower in the weighted benchmark,
especially repaint and ASCII scrolling. The generic quad helper remains better,
likely because the compiler optimizes the shared path effectively and the larger
specialized code hurts layout/cache behavior.

# Failed experiment: cache scaled color glyph dimensions

## Hypothesis

Color emoji drawing recomputes the scaled draw width/height for every color glyph
cell using several floating-point operations and rounds. Emoji workloads reuse the
same color glyphs many times at the same cell dimensions, so caching the last
scaled dimensions in each `GpuGlyph` could reduce per-cell CPU work while
preserving fractional scaling and GPU emoji rendering.

## Patch summary

In `render/gpu.c`, the experiment added cached draw-size fields to `GpuGlyph`:

```c
int drawcellw, drawrowh, drawdw, drawdh;
```

and a helper:

```c
static void gpucolorglyphsize(GpuGlyph *g, int cellw, int rowh, int *dw, int *dh)
```

which recomputed the accepted scale formula only when the cell width or row height
changed. Both `gpudrawline()` and `gpudrawcell()` used this helper for color glyphs.
The cache key included the actual fractional-scaled pixel cell width and row
height, so resize/fractional-scaling behavior was preserved without special
llvmpipe detection.

All rendering still used the actual GPU path, existing glyph/color atlases,
triangle batches, color emoji texture blending, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard. It did not fallback to Xft
or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/color-glyph-size-cache/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.849293`  
Relative score: `0.976x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8295 | 1.6829 | 1.8519 | 0.988x | 1.027x | 1.000x |
| repaint | 0.8073 | 1.2607 | 1.8323 | 0.954x | 1.050x | 1.001x |
| scroll_ascii | 0.9490 | 1.0222 | 1.8636 | 0.963x | 1.037x | 0.999x |
| scroll_unicode | 0.9366 | 1.0763 | 1.8303 | 0.984x | 1.031x | 1.000x |
| scroll_emoji | 1.1455 | 0.7653 | 1.8688 | 1.010x | 0.992x | 0.999x |

## Decision

Rejected and reverted.

The cache gave a small emoji wall improvement, but the added `GpuGlyph` fields and
helper branch hurt higher-weight cursor/repaint/ASCII workloads and lowered the
weighted score substantially. The accepted direct color-glyph scale computation is
better under this benchmark.

# Failed experiment: crop monochrome glyph atlas rectangles

## Hypothesis

The renderer already crops color emoji glyphs to their non-transparent bounding
box before uploading and drawing them. Monochrome glyph bitmaps often include
transparent side bearings or top/bottom rows. Cropping grayscale/mono glyphs to
non-zero final alpha could reduce uploaded atlas area and textured fragments for
llvmpipe, especially with the accepted alpha-test path.

## Patch summary

In `render/gpu.c`, extended glyph cropping in `gpuglyph()`:

- scanned `FT_PIXEL_MODE_GRAY` glyphs after `gpualpha()` and `FT_PIXEL_MODE_MONO`
  glyph bitmasks for non-zero alpha,
- adjusted `g->w`, `g->h`, `g->left`, and `g->top` to preserve glyph placement,
- uploaded only the cropped region for monochrome glyphs, including proper crop
  offsets for gray and mono source rows.

The actual GPU renderer path, accepted alpha test, solid no-blend path, triangle
batches, fractional scaling, glyph atlas rendering, and color emoji behavior were
otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/crop-monochrome-glyphs/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.751418`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6567 | 2.4505 | 1.8526 | 1.006x | 1.014x | 1.001x |
| repaint | 0.7350 | 1.4718 | 1.8328 | 0.980x | 1.020x | 1.001x |
| scroll_ascii | 0.9351 | 1.0699 | 1.8653 | 1.015x | 0.985x | 1.001x |
| scroll_unicode | 0.8817 | 1.1770 | 1.8267 | 0.972x | 1.050x | 0.999x |
| scroll_emoji | 1.0882 | 0.8314 | 1.8704 | 0.999x | 1.011x | 1.000x |

## Decision

Rejected and reverted.

Cropping helped ASCII wall time, but the added glyph-load scanning/cropping cost
and smaller quads did not improve the weighted result. It materially regressed
repaint and Unicode scrolling and worsened CPU in the high-priority workloads.
Keep monochrome glyph handling simple and rely on the accepted alpha test to skip
transparent texels.

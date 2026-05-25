# Failed experiment: upload color glyphs with GL_UNPACK_ROW_LENGTH

## Hypothesis

Color emoji glyph uploads currently copy each cropped BGRA glyph into a temporary
tightly-packed buffer before `glTexSubImage2D()`. For the common positive-pitch
FreeType bitmap case, OpenGL's `GL_UNPACK_ROW_LENGTH` could upload directly from
the original bitmap with a row stride and crop offset, avoiding one memcpy and one
temporary allocation per color glyph.

## Patch summary

In `render/gpu.c`, the experiment initialized row-length unpack state:

```c
glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
```

and changed the color-glyph branch of `gpuglyph()` so positive-pitch bitmaps use:

```c
glPixelStorei(GL_UNPACK_ROW_LENGTH, bm->pitch / 4);
glTexSubImage2D(..., bm->buffer + cropy * bm->pitch + cropx * 4);
glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
```

Negative-pitch bitmaps kept the existing tight-copy fallback. Monochrome/gray
uploads were unchanged.

This preserved the actual GPU renderer path, glyph/emoji visual behavior,
fractional scaling, triangle batches, alpha test, solid no-blend behavior,
accepted clear-color cache, and cleared-background skip. It did not fallback to
Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/color-rowlength-upload/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/color-rowlength-upload-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.858094`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8268 | 1.7098 | 1.8504 | 0.994x | 1.021x | 1.000x |
| repaint | 0.8311 | 1.2286 | 1.8312 | 0.992x | 1.009x | 1.001x |
| scroll_ascii | 0.9711 | 1.0010 | 1.8617 | 0.979x | 1.006x | 0.998x |
| scroll_unicode | 0.9450 | 1.0592 | 1.8287 | 0.994x | 0.997x | 1.001x |
| scroll_emoji | 1.1436 | 0.7704 | 1.8701 | 0.998x | 0.996x | 1.001x |

## Decision

Rejected and reverted.

The first run was almost tied with accepted (`0.864938`), but validation regressed
clearly. Direct strided color uploads did not improve emoji enough and hurt the
higher-weight cursor/repaint/ASCII workloads. The accepted tight-copy color upload
path remains better under this llvmpipe benchmark.

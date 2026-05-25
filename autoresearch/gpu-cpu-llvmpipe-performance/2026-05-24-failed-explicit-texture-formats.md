# Failed experiment: explicit texture internal formats

## Hypothesis

The GPU renderer uploads the glyph atlas with unsized legacy internal formats
(`GL_ALPHA` for monochrome glyphs and `GL_RGBA` for color emoji). Mesa/llvmpipe
may have to choose concrete storage formats internally. Requesting explicit sized
formats (`GL_ALPHA8` and `GL_RGBA8`) might reduce format-selection/conversion
work while preserving atlas contents and sampling behavior.

## Patch summary

In `render/gpu.c`, changed atlas allocation calls from:

```c
glTexImage2D(..., GL_ALPHA, ..., GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
glTexImage2D(..., GL_RGBA,  ..., GL_BGRA,  GL_UNSIGNED_BYTE, NULL);
```

to:

```c
glTexImage2D(..., GL_ALPHA8, ..., GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
glTexImage2D(..., GL_RGBA8,  ..., GL_BGRA,  GL_UNSIGNED_BYTE, NULL);
```

This preserved the actual GPU renderer path, glyph/emoji atlas layout, texture
filtering, fractional scaling, triangle batches, alpha test, solid no-blend
behavior, the accepted clear-color cache, and cleared-background skip. It did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/explicit-texture-formats/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/explicit-texture-formats-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.862537`  
Relative score: `0.997x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8420 | 1.6534 | 1.8508 | 1.012x | 0.987x | 1.000x |
| repaint | 0.8319 | 1.2282 | 1.8307 | 0.993x | 1.008x | 1.000x |
| scroll_ascii | 0.9727 | 0.9964 | 1.8635 | 0.981x | 1.002x | 0.999x |
| scroll_unicode | 0.9274 | 1.0730 | 1.8299 | 0.976x | 1.010x | 1.001x |
| scroll_emoji | 1.1420 | 0.7678 | 1.8685 | 0.997x | 0.992x | 1.000x |

## Decision

Rejected and reverted.

The initial run was slightly positive (`0.866338`), but validation failed to
reproduce it. The validated weighted score fell below the accepted clear-color
state cache, and repaint plus scrolling wall ratios regressed despite a cursor
wall improvement. The legacy internal formats remain better for this benchmark
state.

# Failed experiment: draw color-text batches with constant white color

## Hypothesis

Color emoji batches are always drawn white so that the BGRA color texture supplies
the visible color. The accepted `GpuVertex` format still stores four float color
components for each color-text vertex and enables the color client array for
`gpu.ctext` / `gpu.octext`. Disabling the color array for `textured == 2` and using
constant `glColor4f(1,1,1,1)` could reduce llvmpipe client-array bandwidth for
emoji draws without changing rendering.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` was changed so color-text batches disable the
color client array and use constant white:

```c
if (textured == 2) {
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
} else {
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, sizeof(GpuVertex), coff);
}
```

Vertex and texture-coordinate arrays, triangle batches, blending, alpha test,
actual GPU path, fractional scaling, accepted clear-color cache, cleared-background
skip, and accepted vimnav row guard were otherwise unchanged. The benchmark still
compared same-source Xft and GPU paths under llvmpipe; it did not fallback to Xft
or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/ctext-constant-color/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.847379`  
Relative score: `0.974x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8296 | 1.7006 | 1.8503 | 0.988x | 1.038x | 0.999x |
| repaint | 0.7956 | 1.2772 | 1.8302 | 0.940x | 1.064x | 1.000x |
| scroll_ascii | 0.9679 | 1.0097 | 1.8636 | 0.982x | 1.024x | 0.999x |
| scroll_unicode | 0.9299 | 1.0652 | 1.8296 | 0.977x | 1.020x | 1.000x |
| scroll_emoji | 1.1434 | 0.7706 | 1.8740 | 1.008x | 0.999x | 1.001x |

## Decision

Rejected and reverted.

Emoji wall improved slightly, but disabling the color array for color-text batches
regressed repaint heavily and reduced the total score. The extra state branch and
constant-color path are not worthwhile under llvmpipe; keep the accepted per-vertex
white color data for color-text batches.

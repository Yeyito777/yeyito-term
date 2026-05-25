# Failed experiment: disable GL point/line/polygon smoothing in GPU init

## Hypothesis

The renderer draws triangle batches for rectangles and textured glyph quads, and it
never intentionally uses point, line, or polygon smoothing. Explicitly disabling
`GL_POINT_SMOOTH`, `GL_LINE_SMOOTH`, and `GL_POLYGON_SMOOTH` might avoid any
unexpected llvmpipe smoothing state cost without changing intended output.

## Patch summary

In `render/gpu.c`, the experiment added three GL state setup calls in `gpuinit()`
after pixel unpack setup:

```c
glDisable(GL_POINT_SMOOTH);
glDisable(GL_LINE_SMOOTH);
glDisable(GL_POLYGON_SMOOTH);
```

No batching, glyph/emoji rendering, fractional scaling, accepted clear-color cache,
cleared-background skip, or accepted vimnav row guard was changed. The benchmark
still compared same-source Xft and actual GPU paths under llvmpipe; it did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-smooth/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.866296`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8231 | 1.7277 | 1.8458 | 0.981x | 1.055x | 0.997x |
| repaint | 0.8201 | 1.2424 | 1.8297 | 0.969x | 1.035x | 1.000x |
| scroll_ascii | 1.0037 | 0.9762 | 1.8649 | 1.018x | 0.990x | 1.000x |
| scroll_unicode | 0.9324 | 1.0779 | 1.8284 | 0.980x | 1.032x | 0.999x |
| scroll_emoji | 1.2399 | 0.7094 | 1.8688 | 1.093x | 0.919x | 0.999x |

## Decision

Rejected and reverted.

Smoothing disables improved ASCII and emoji wall ratios, but they regressed the
higher-weight cursor and repaint workloads and lowered the total score. Keep the
accepted GL state setup.

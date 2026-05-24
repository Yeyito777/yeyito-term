# Failed experiment: disable GL dithering

## Hypothesis

OpenGL dithering is enabled by default, but st renders into a true-color visual
and does not need dithering for the terminal's solid colors or glyph textures.
Disabling `GL_DITHER` during GPU initialization might reduce llvmpipe rasterizer
work without changing visible behavior in the normal true-color path.

## Patch summary

In `render/gpu.c`, the experiment added:

```c
glDisable(GL_DITHER);
```

after texture unpack setup and before the normal blend state setup in `gpuinit()`.
The same patch also fixed a nearby indentation typo around the accepted atlas
size assignment while testing.

The renderer stayed on the actual GPU path with triangle batches, fractional
scaling, glyph atlases, color emoji, and the accepted solid-batch no-blend
behavior. This was not an Xft fallback or `gpudraw` bypass.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-dither/result.json`

## Result versus accepted disable-solid-blend state

Accepted score: `0.750395`  
Experiment score: `0.749224`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6483 | 2.4352 | 1.8503 | 1.005x | 0.988x | 1.000x |
| repaint | 0.7509 | 1.4421 | 1.8314 | 1.009x | 1.004x | 1.002x |
| scroll_ascii | 0.9023 | 1.1046 | 1.8628 | 0.978x | 1.022x | 0.999x |
| scroll_unicode | 0.8820 | 1.1726 | 1.8280 | 0.997x | 1.005x | 1.001x |
| scroll_emoji | 1.0813 | 0.8266 | 1.8671 | 0.997x | 1.000x | 0.998x |

## Decision

Rejected and reverted.

The result was very close but still below the accepted weighted score, and ASCII
scrolling regressed materially. The high-priority workloads improved a little,
but the acceptance rule requires overall weighted-score improvement without
material regressions, so this does not qualify.

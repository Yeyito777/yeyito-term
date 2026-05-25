# Failed experiment: disable GL_MULTISAMPLE in GPU init

## Hypothesis

The terminal renderer draws pixel-aligned 2D rectangles and textured glyph quads;
it does not need multisampling. If the context or driver had multisampling enabled
by default, explicitly disabling `GL_MULTISAMPLE` could remove unnecessary llvmpipe
rasterization work without changing intended output.

## Patch summary

In `render/gpu.c`, the experiment added one GL state setup call in `gpuinit()` after
pixel unpack setup:

```c
glDisable(GL_MULTISAMPLE);
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-multisample/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.860546`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8431 | 1.6339 | 1.8495 | 1.004x | 0.997x | 0.999x |
| repaint | 0.8154 | 1.2571 | 1.8292 | 0.963x | 1.047x | 1.000x |
| scroll_ascii | 0.9900 | 0.9834 | 1.8654 | 1.005x | 0.997x | 1.000x |
| scroll_unicode | 0.9441 | 1.0712 | 1.8301 | 0.992x | 1.026x | 1.000x |
| scroll_emoji | 1.1248 | 0.7759 | 1.8675 | 0.992x | 1.006x | 0.998x |

## Decision

Rejected and reverted.

Disabling multisample improved cursor and ASCII wall slightly, but repaint and
emoji regressed and the weighted score fell below accepted. Keep the accepted GL
state setup.

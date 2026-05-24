# Failed experiment: skip per-frame glXMakeCurrent

## Hypothesis

The GPU context is made current during initialization and normal drawing should
not intentionally switch to another context. The GPU branch of `xstartdraw()`
still calls `glXMakeCurrent()` every frame before resizing/clearing/drawing.
Skipping that redundant-looking call might avoid GLX dispatch overhead under
llvmpipe while preserving the renderer path.

## Patch summary

In `x.c`, removed the per-frame call:

```c
glXMakeCurrent(xw.dpy, xw.win, gpu.ctx);
```

from the `gpu.active` branch of `xstartdraw()`.

This preserved the actual GPU renderer codepath, full-frame clear semantics,
accepted clear-color cache, cleared-background skip, triangle batches, alpha
test, solid no-blend behavior, and text/emoji rendering. It did not fallback to
Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-frame-makecurrent/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.857758`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8369 | 1.6567 | 1.8493 | 1.006x | 0.989x | 0.999x |
| repaint | 0.8224 | 1.2494 | 1.8326 | 0.981x | 1.026x | 1.001x |
| scroll_ascii | 0.9700 | 1.0068 | 1.8655 | 0.978x | 1.012x | 1.000x |
| scroll_unicode | 0.9257 | 1.0778 | 1.8271 | 0.974x | 1.014x | 1.000x |
| scroll_emoji | 1.1506 | 0.7702 | 1.8686 | 1.004x | 0.996x | 1.000x |

## Decision

Rejected and reverted.

Although cursor and emoji wall ratios improved slightly, the total score fell and
repaint/scrolling regressed. Keeping the explicit per-frame `glXMakeCurrent()` is
safer and faster overall in the accepted benchmark state.

# Failed experiment: remove redundant make-current inside GPU resize path

## Hypothesis

`xstartdraw()` already calls `glXMakeCurrent()` before `gpuresize()`. When the
window size changes, `gpuresize()` calls `glXMakeCurrent()` again before updating
viewport/projection state. Removing the inner call should preserve correctness
because the context is already current and might reduce resize/startup overhead in
benchmark processes.

## Patch summary

In `render/gpu.c`, removed this line from the `gpu.vw != win.w || gpu.vh != win.h`
branch of `gpuresize()`:

```c
glXMakeCurrent(xw.dpy, xw.win, gpu.ctx);
```

All viewport/projection updates, clear behavior, batching, glyph/emoji rendering,
fractional scaling, accepted clear-color cache, cleared-background skip, and
accepted vimnav row guard were otherwise unchanged. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-resize-makecurrent/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.863402`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8342 | 1.6557 | 1.8471 | 0.994x | 1.011x | 0.997x |
| repaint | 0.8446 | 1.2459 | 1.8308 | 0.998x | 1.038x | 1.000x |
| scroll_ascii | 0.9781 | 0.9986 | 1.8624 | 0.992x | 1.013x | 0.998x |
| scroll_unicode | 0.9399 | 1.0698 | 1.8297 | 0.988x | 1.025x | 1.000x |
| scroll_emoji | 1.1381 | 0.7708 | 1.8666 | 1.004x | 0.999x | 0.997x |

## Decision

Rejected and reverted.

Although the extra make-current appears redundant, removing it did not improve the
benchmark and lowered the weighted score. Keep the conservative resize path.

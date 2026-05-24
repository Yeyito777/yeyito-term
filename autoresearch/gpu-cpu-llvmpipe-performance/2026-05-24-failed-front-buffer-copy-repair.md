# Failed experiment: repair back buffer by copying front buffer

## Hypothesis

On the benchmark GLX/Xephyr stack, the GPU path lacks usable buffer age and must
currently clear and redraw a complete frame after each swap. Instead of full
redraw, copying the previous front buffer into the back buffer before drawing the
current dirty rows could preserve the previous frame and avoid `tfulldirt()` on
most frames while remaining on the actual GPU renderer path.

## Patch summary

The experiment split the full-clear path in `xstartdraw()`:

- still used the accepted full clear + `tfulldirt()` path when `gpu.needclear`
  was set,
- otherwise, when double-buffered buffer age was unavailable/invalid, copied the
  front buffer to the back buffer with:

```c
glReadBuffer(GL_FRONT);
glDrawBuffer(GL_BACK);
glRasterPos2i(0, win.h);
glCopyPixels(0, 0, win.w, win.h, GL_COLOR);
```

and avoided `tfulldirt()` for that frame.

The experiment preserved the actual GPU renderer for subsequent dirty drawing and
did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/front-buffer-copy-repair/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.728086`  
Relative score: `0.842x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.7055 | 2.1963 | 1.8900 | 0.848x | 1.311x | 1.021x |
| repaint | 0.6466 | 1.7241 | 2.0043 | 0.772x | 1.415x | 1.095x |
| scroll_ascii | 0.9066 | 1.1060 | 1.9426 | 0.914x | 1.112x | 1.042x |
| scroll_unicode | 0.8576 | 1.1981 | 1.9032 | 0.902x | 1.127x | 1.041x |
| scroll_emoji | 1.0266 | 0.8781 | 1.9438 | 0.896x | 1.135x | 1.040x |

## Decision

Rejected and reverted.

The front-buffer copy path was much slower under llvmpipe/Xephyr than full clear
plus redraw. It also increased RSS materially. The accepted full-clear path with
default-background skip remains far better.

# Autoresearch: GPU CPU performance under Mesa llvmpipe

Topic: the experimental GPU renderer when hardware GPU acceleration is unavailable and Mesa llvmpipe is used as the CPU rasterizer.

The objective benchmark is `benchmark_llvmpipe.py`.  It compares two builds from the same source tree:

- `xft`: `gpudraw = 0`
- `gpu_llvmpipe`: `gpudraw = 1` with `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`, and `LP_NUM_THREADS=1`

## Acceptance rule

Experiments should be kept only when they improve the weighted benchmark score without materially regressing the high-priority `cursor_updates` or `repaint` workloads.

Metric weights:

- wall time: 70%
- CPU time: 25%
- RSS: 5%

Workload weights:

- cursor updates: 35%
- repaint: 30%
- ASCII scroll: 15%
- Unicode scroll: 10%
- emoji scroll: 10%

Failures and successes are permanently logged in this directory.  Failed code is reverted; the failure log is committed.  Successful code and its success log are committed together.

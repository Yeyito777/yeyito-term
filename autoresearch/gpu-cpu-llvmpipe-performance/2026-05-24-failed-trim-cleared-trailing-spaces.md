# Failed experiment: trim trailing spaces on cleared frames

## Hypothesis

On a fully-cleared frame, trailing cells that are spaces with default background
are already visually represented by the clear color. `tlinelen(y)` identifies the
last non-space cell. Clamping `gpudrawline()`'s dirty span to `tlinelen(y)` for
simple cleared rows might skip per-cell work for trailing blanks without adding a
per-cell early-exit branch.

## Patch summary

In `render/gpu.c`, changed `gpudrawline()` so that when the frame was fully
cleared and row-wide effects are inactive:

```c
if (gpu.clearedframe && !selactive && !searchactive &&
    !IS_SET(MODE_REVERSE) && !debug_mode && y != vimline) {
    int len = tlinelen(y);
    if (x2 > len)
        x2 = MAX(x1, len);
}
```

The actual GPU renderer path, accepted full-clear default-background skip,
accepted alpha test, solid no-blend behavior, triangle batches, fractional
scaling, glyph atlas rendering, and color emoji path were otherwise unchanged.
This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/trim-cleared-trailing-spaces/result.json`

## Result versus accepted skipped-cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.860862`  
Relative score: `0.997x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8413 | 1.6687 | 1.8497 | 1.002x | 1.008x | 1.000x |
| repaint | 0.8185 | 1.2335 | 1.8309 | 0.985x | 1.002x | 0.999x |
| scroll_ascii | 0.9749 | 1.0010 | 1.8638 | 0.996x | 1.004x | 0.999x |
| scroll_unicode | 0.9452 | 1.0779 | 1.8311 | 1.010x | 1.006x | 1.002x |
| scroll_emoji | 1.1530 | 0.7661 | 1.8686 | 1.008x | 1.004x | 0.999x |

## Decision

Rejected and reverted.

The extra `tlinelen()` scan helped emoji/unicode a little but regressed repaint,
ASCII, CPU, and the weighted score. The accepted single-pass row renderer with
background-run skipping remains better.

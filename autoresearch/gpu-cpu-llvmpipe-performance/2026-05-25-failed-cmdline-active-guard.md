# Failed experiment: guard cmdline_active() with vimnav mode in GPU cursor draw

## Hypothesis

`gpudrawcursor()` checks `cmdline_active()` every cursor draw so it can suppress
normal cursor rendering while the vim-style command line is active. The command
line is entered from vimnav, and the benchmark normally has `vimnav.mode == 0`.
Guarding the helper call with `vimnav.mode` could avoid a cursor hot-path function
call while preserving behavior for active vimnav/cmdline sessions.

## Patch summary

In `render/gpu.c`, the experiment changed:

```c
if ((IS_SET(MODE_HIDE) && !vimnav.forced) || cmdline_active())
    return;
```

to:

```c
if ((IS_SET(MODE_HIDE) && !vimnav.forced) || (vimnav.mode && cmdline_active()))
    return;
```

Cursor rendering, overlay ordering, glyph/emoji rendering, fractional scaling,
triangle batches, alpha test, solid no-blend behavior, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cmdline-active-guard/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.862055`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8333 | 1.6627 | 1.8496 | 0.993x | 1.015x | 0.999x |
| repaint | 0.8353 | 1.2075 | 1.8323 | 0.987x | 1.006x | 1.001x |
| scroll_ascii | 0.9791 | 1.0187 | 1.8632 | 0.993x | 1.033x | 0.999x |
| scroll_unicode | 0.9514 | 1.0578 | 1.8290 | 1.000x | 1.013x | 1.000x |
| scroll_emoji | 1.1258 | 0.7807 | 1.8683 | 0.993x | 1.012x | 0.998x |

## Decision

Rejected and reverted.

The guard did not improve the cursor workload and lowered the weighted score.
The extra condition is not worthwhile in the current GPU cursor path.

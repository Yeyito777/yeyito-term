# Failed experiment: carry cleared-background skip flag in row runs

## Hypothesis

The accepted skipped-cleared-background patch decides whether to emit a background
run by comparing the closed run color against the default background. Since the
hot path already knows when a cell used the direct default-background fast path,
carrying a boolean `runskip` flag with the current background run could avoid the
final color comparison and make the skip condition more explicit.

## Patch summary

In `render/gpu.c`, changed `gpudrawline()` to:

- compute `bgisdefault` while resolving each cell,
- track `runskip = gpu.clearedframe && bgisdefault` for each background run,
- merge background runs only when both color and skip flag match,
- skip emitting a run based on `runskip` instead of comparing `runbg` with the
  default background color when the run closes.

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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cleared-background-run-skip-flag/result.json`

## Result versus accepted skipped-cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.861705`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8463 | 1.6613 | 1.8515 | 1.007x | 1.003x | 1.001x |
| repaint | 0.8314 | 1.2372 | 1.8313 | 1.000x | 1.005x | 1.000x |
| scroll_ascii | 0.9816 | 1.0051 | 1.8629 | 1.003x | 1.008x | 0.998x |
| scroll_unicode | 0.9309 | 1.0757 | 1.8321 | 0.995x | 1.004x | 1.002x |
| scroll_emoji | 1.1195 | 0.7745 | 1.8685 | 0.978x | 1.015x | 0.999x |

## Decision

Rejected and reverted.

The boolean skip flag slightly improved cursor and kept repaint wall essentially
flat, but it regressed the weighted score and emoji, and it made the run merging
logic more complex. The accepted color-comparison form is simpler and measured
slightly better overall.

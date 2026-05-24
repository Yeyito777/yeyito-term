# Failed experiment: split background runs around cleared default spans

## Hypothesis

After accepting skipped default-background runs on fully-cleared frames, the
renderer still lets default-background cells participate in the current background
run and only skips a run if the whole closed run is the default background. If a
row contains small non-default islands, splitting the run whenever a cleared
frame sees a default-background cell could avoid accidentally carrying default
cells inside a later non-default run.

## Patch summary

In `render/gpu.c`, changed `gpudrawline()` so that on a fully-cleared frame:

- default-background cells close any active non-default background run and emit it,
- default-background cells do not start/extend a background run,
- non-default cells continue to form normal background runs.

This preserved the actual GPU renderer path, accepted alpha test, solid no-blend
behavior, triangle batches, fractional scaling, glyph atlas rendering, and color
emoji path. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/split-cleared-default-bg-runs/result.json`

## Result versus accepted skipped-cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.861478`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8447 | 1.6305 | 1.8505 | 1.006x | 0.985x | 1.000x |
| repaint | 0.8218 | 1.2786 | 1.8304 | 0.989x | 1.039x | 0.999x |
| scroll_ascii | 0.9758 | 0.9896 | 1.8666 | 0.997x | 0.993x | 1.000x |
| scroll_unicode | 0.9446 | 1.0569 | 1.8301 | 1.009x | 0.986x | 1.001x |
| scroll_emoji | 1.1455 | 0.7757 | 1.8701 | 1.001x | 1.017x | 1.000x |

## Decision

Rejected and reverted.

The split-run logic slightly improved cursor/unicode wall ratios, but it regressed
repaint and the weighted score. The accepted simpler rule—skip only whole
resolved default-background runs after a full clear—is faster and clearer.

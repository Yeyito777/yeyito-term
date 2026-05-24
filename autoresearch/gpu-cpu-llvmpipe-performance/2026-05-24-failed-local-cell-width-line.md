# Failed experiment: local row cell-width computation

## Hypothesis

`gpudrawline()` calls `gpucellx()` / `gpucellright()` for every cell, and those
helpers recompute the scaled cell width through `gpucellw()` / `gpuxscale()`.
Caching the row's scaled cell width locally and computing rounded cell positions
from that value might reduce per-cell CPU cost while preserving fractional
scaling math.

## Patch summary

In `render/gpu.c`, `gpudrawline()` was changed to:

- compute `double cw = gpucellw()` once per row,
- compute `cellx`, `cellnext`, and wide-cell right edge directly with the same
  `borderpx + gpuround(x * cw)` formula,
- reuse `cellnext - cellx` for underline/strikethrough widths.

This stayed on the actual GPU renderer path and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/local-cell-width-line/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.702869`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5854 | 2.8753 | 1.8511 | 0.994x | 1.008x | 1.000x |
| repaint | 0.6974 | 1.5729 | 1.8307 | 0.988x | 1.025x | 1.001x |
| scroll_ascii | 0.8610 | 1.1767 | 1.8617 | 0.947x | 1.055x | 0.999x |
| scroll_unicode | 0.8694 | 1.2024 | 1.8259 | 1.022x | 0.967x | 1.000x |
| scroll_emoji | 1.0622 | 0.8468 | 1.8698 | 0.992x | 0.999x | 1.000x |

## Decision

Rejected and reverted.

Despite being algebraically equivalent, the local computation regressed weighted
score, repaint, and especially ASCII scrolling. The helper calls were not the
important llvmpipe bottleneck here, and the compiler may already optimize the
simple scaling path sufficiently. Keep the clearer helper-based code.

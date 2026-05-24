# Failed experiment: skip background tracking for all-default rows on cleared frames

## Hypothesis

After accepting skipped default-background draws on fully-cleared frames, rows
whose entire dirty span resolves to the default background still pay the CPU cost
of building and comparing a background run that will be skipped at the end. If a
row is known to contain only default-background cells and no row-wide effects,
`gpudrawline()` could skip background-run tracking entirely and only draw glyphs /
decorations.

## Patch summary

In `render/gpu.c`, `gpudrawline()` was changed to:

- compute `skipbg` on fully-cleared frames when selection, search, reverse-video,
  debug prompt, and vimnav-current-line effects are inactive,
- scan the dirty span for cells whose background is not `defaultbg` or whose mode
  includes selected/match/reverse attributes,
- when `skipbg` stays true, bypass background run creation/merge/flush logic for
  the row while preserving glyph, color emoji, underline, and strikethrough
  rendering.

The actual GPU renderer path, accepted full-clear default-background skip, alpha
test, solid no-blend behavior, triangle batches, fractional scaling, glyph atlas
rendering, and color emoji path were otherwise unchanged. This did not fallback
to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-default-bg-rows/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.889308`  
Relative score: `1.030x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8358 | 1.6643 | 1.8473 | 0.995x | 1.005x | 0.998x |
| repaint | 0.8006 | 0.7886 | 1.8328 | 0.963x | 0.641x | 1.000x |
| scroll_ascii | 0.9853 | 0.9947 | 1.8677 | 1.007x | 0.998x | 1.001x |
| scroll_unicode | 0.9507 | 1.0549 | 1.8267 | 1.016x | 0.984x | 0.999x |
| scroll_emoji | 1.1201 | 0.7850 | 1.8704 | 0.979x | 1.029x | 1.000x |

## Decision

Rejected and reverted.

The weighted score was higher, mostly because repaint CPU ratio improved sharply
in this particular run, but it materially regressed repaint wall time by about
3.7% and slightly regressed cursor wall time. The acceptance rule prioritizes
wall time and forbids material cursor/repaint regressions, so this is not a safe
keep. It also adds a second row scan and more branching to an already hot path.

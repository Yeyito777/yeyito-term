# Failed experiment: skip cleared default-background spaces entirely

## Hypothesis

After accepting skipped default-background runs on fully-cleared frames, cells
that are literal spaces with the default background and no underline/strike have
no visible text or decoration. On a cleared frame, those cells should need no GPU
work at all. Skipping them before foreground resolution and glyph handling might
reduce per-cell CPU work in sparse rows.

## Patch summary

In `render/gpu.c`, changed the default-background fast path in `gpudrawline()` so
that when `gpu.clearedframe` is true and a cell is:

- `g.u == ' '`,
- resolved through the default-background fast path,
- not underline/strikethrough,

then the renderer closes any active background run, skips batching default
background, and continues before foreground resolution / glyph lookup.

The actual GPU renderer path, accepted full-clear default-background skip,
accepted alpha test, solid no-blend behavior, triangle batches, fractional
scaling, glyph atlas rendering, and color emoji path were otherwise unchanged.
This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-default-spaces/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-default-spaces-validate/result.json`

## Validation result versus accepted skipped-cleared-background state

Accepted score: `0.863260`  
Initial score: `0.864265`  
Validation score: `0.862710`  
Validation relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8555 | 1.6293 | 1.8504 | 1.019x | 0.984x | 1.000x |
| repaint | 0.8252 | 1.2436 | 1.8296 | 0.993x | 1.011x | 0.999x |
| scroll_ascii | 0.9625 | 1.0062 | 1.8637 | 0.983x | 1.009x | 0.998x |
| scroll_unicode | 0.9378 | 1.0797 | 1.8304 | 1.002x | 1.008x | 1.001x |
| scroll_emoji | 1.1351 | 0.7784 | 1.8696 | 0.992x | 1.021x | 1.000x |

## Decision

Rejected and reverted.

The initial run was slightly positive, but validation fell below the accepted
score. The change helped cursor, but it regressed repaint, ASCII scrolling, and
emoji versus the accepted cleared-background state. It also complicates the row
loop by adding another early-exit case. Keep the simpler accepted background-run
skip only.

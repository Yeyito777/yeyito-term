# Failed experiment: skip old cursor overlay when old row was already dirty

## Hypothesis

`gpudrawcursor()` normally draws an overlay for the old cursor cell to restore the
cell underneath the previous cursor, then draws the new cursor. If the old cursor
row was already dirty at the start of `draw()`, `drawregion()` redraws that cell
before cursor overlay rendering, so the explicit old-cursor overlay should be
redundant. Skipping it in that case could reduce cursor workload overhead while
preserving correctness.

## Patch summary

The experiment threaded an `olddrawn` flag from `draw()` to `xdrawcursor()` and
then to the GPU cursor helper:

```c
olddrawn = BETWEEN(term.ocy, 0, term.row - 1) && term.dirty[term.ocy];
...
xdrawcursor(..., olddrawn);
```

and in `gpudrawcursor()`:

```c
if (!olddrawn)
    gpudrawcell(og, ox, oy, 1);
```

The Xft cursor path ignored the flag. The actual GPU renderer path, dirty-row
redraw behavior, fractional scaling, GPU text/emoji behavior, triangle batches,
alpha test, solid no-blend behavior, accepted clear-color cache, and
cleared-background skip were otherwise unchanged. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-dirty-old-cursor/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-dirty-old-cursor-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.860005`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8387 | 1.6552 | 1.8510 | 1.009x | 0.988x | 1.000x |
| repaint | 0.8233 | 1.2363 | 1.8318 | 0.982x | 1.015x | 1.001x |
| scroll_ascii | 0.9640 | 1.0195 | 1.8660 | 0.972x | 1.025x | 1.001x |
| scroll_unicode | 0.9426 | 1.0670 | 1.8296 | 0.992x | 1.004x | 1.001x |
| scroll_emoji | 1.1195 | 0.7092 | 1.8719 | 0.977x | 0.917x | 1.002x |

## Decision

Rejected and reverted.

The initial run looked positive (`0.870287`) due mostly to emoji noise and a small
cursor-wall improvement, but validation failed to reproduce the gain. The
validated score fell below accepted and high-priority repaint wall regressed
materially. The cursor API complexity is not justified.

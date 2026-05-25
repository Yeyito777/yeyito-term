# Failed experiment: skip old cursor restore when cursor stays in the same cell

## Hypothesis

`gpudrawcursor()` restores the old cursor cell and then draws the new cursor. When
the old cursor coordinates are the same as the new cursor coordinates, restoring
the old cell immediately before drawing the new cursor should be redundant. Skipping
that restore might reduce cursor overlay work without affecting cursor rendering.

## Patch summary

In `render/gpu.c`, the experiment changed:

```c
gpudrawcell(og, ox, oy, 1);
```

to:

```c
if (ox != cx || oy != cy)
    gpudrawcell(og, ox, oy, 1);
```

All other cursor drawing, normal line rendering, clear behavior, batch handling,
glyph/emoji rendering, fractional scaling, alpha test, solid no-blend behavior,
accepted clear-color cache, and cleared-background skip were unchanged.

This preserved the actual GPU renderer path and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/same-cursor-skip-old/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.863736`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8379 | 1.6599 | 1.8505 | 1.008x | 0.991x | 1.000x |
| repaint | 0.8347 | 1.2244 | 1.8309 | 0.996x | 1.005x | 1.000x |
| scroll_ascii | 0.9896 | 0.9912 | 1.8673 | 0.998x | 0.996x | 1.001x |
| scroll_unicode | 0.9332 | 1.0816 | 1.8273 | 0.982x | 1.018x | 1.000x |
| scroll_emoji | 1.1371 | 0.7725 | 1.8690 | 0.993x | 0.999x | 1.000x |

## Decision

Rejected and reverted.

The same-cell guard gave a small cursor-wall improvement, but unicode and emoji
scrolling regressed and the weighted score stayed below the accepted clear-color
cache state. Keep the simpler unconditional old-cursor restore.

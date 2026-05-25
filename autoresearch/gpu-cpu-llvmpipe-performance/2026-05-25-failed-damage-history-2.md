# Failed experiment: reduce GPU damage history to 2 frames

## Hypothesis

The accepted GPU renderer keeps `GPU_DAMAGE_HISTORY` at 4 rows of history for
GLX buffer-age repair. In the llvmpipe/Xephyr benchmark, back-buffer ages may not
need four generations. Reducing the history depth to 2 could cut damage-history
memory, per-frame clears, and aged-buffer repair iteration overhead while still
preserving normal double-buffer repair for ages up to 2.

## Patch summary

In `x.c`, changed:

```c
#define GPU_DAMAGE_HISTORY 4
```

to:

```c
#define GPU_DAMAGE_HISTORY 2
```

The existing clear fallback remains intact when the reported back-buffer age is 0
or greater than the history depth, so rendering correctness is preserved by doing
a full clear/redraw in unsupported age cases. Actual GPU rendering, batching,
glyph/emoji behavior, fractional scaling, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/damage-history-2/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.864051`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8325 | 1.6260 | 1.8500 | 0.992x | 0.993x | 0.999x |
| repaint | 0.8149 | 1.2742 | 1.8322 | 0.962x | 1.062x | 1.001x |
| scroll_ascii | 0.9565 | 1.0263 | 1.8630 | 0.971x | 1.041x | 0.999x |
| scroll_unicode | 0.9831 | 1.0374 | 1.8313 | 1.033x | 0.994x | 1.001x |
| scroll_emoji | 1.2133 | 0.7268 | 1.8688 | 1.070x | 0.942x | 0.999x |

## Decision

Rejected and reverted.

The smaller damage history improved unicode and emoji wall ratios, but cursor,
repaint, and ASCII regressed and the weighted score stayed below accepted. Keep the
accepted 4-frame damage history.

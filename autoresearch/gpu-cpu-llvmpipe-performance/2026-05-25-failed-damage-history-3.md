# Failed experiment: reduce GPU damage history to 3 frames

## Hypothesis

The accepted GPU renderer keeps four damage-history rows for GLX buffer-age repair.
After a 2-frame history failed, a smaller reduction to three frames might keep most
aged-buffer repair capability while slightly reducing damage-history memory,
per-frame clears, and repair-loop overhead.

## Patch summary

In `x.c`, changed:

```c
#define GPU_DAMAGE_HISTORY 4
```

to:

```c
#define GPU_DAMAGE_HISTORY 3
```

The existing full-clear fallback remains for age 0 or age greater than the history
depth, preserving correctness. Actual GPU rendering, batching, glyph/emoji
behavior, fractional scaling, accepted clear-color cache, cleared-background skip,
and accepted vimnav row guard were otherwise unchanged. It did not fallback to Xft
or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/damage-history-3/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.859227`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8417 | 1.6560 | 1.8476 | 1.003x | 1.011x | 0.998x |
| repaint | 0.8155 | 1.2444 | 1.8346 | 0.963x | 1.037x | 1.002x |
| scroll_ascii | 0.9790 | 0.9940 | 1.8640 | 0.993x | 1.008x | 0.999x |
| scroll_unicode | 0.9380 | 1.0619 | 1.8289 | 0.986x | 1.017x | 1.000x |
| scroll_emoji | 1.1346 | 0.7712 | 1.8682 | 1.001x | 0.999x | 0.998x |

## Decision

Rejected and reverted.

The three-frame history slightly improved cursor and emoji wall ratios, but repaint
regressed significantly and the total score fell below accepted. Keep the accepted
four-frame damage history.

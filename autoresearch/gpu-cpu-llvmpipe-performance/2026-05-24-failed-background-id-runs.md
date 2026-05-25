# Failed experiment: compare background runs by color id

## Hypothesis

`gpudrawline()` compares adjacent background runs with three float comparisons via
`gpucoloreq()`. Since background colors are ultimately resolved from st color ids
(including truecolor ids), returning the resolved background id from `gpuresolve()`
and comparing ids for run merging could reduce per-cell float comparison work in
the hot line renderer.

## Patch summary

The experiment changed `gpuresolve()` in `render/gpu.c` to return the resolved
background id while still filling `fg` and `bg`. `gpudrawline()` tracked `bgid`
and `runbgid` and used:

```c
runbgid == bgid
```

for adjacent background-run merging instead of `gpucoloreq(runbg, bg)`. The
cleared-frame default-background skip still used the original float comparison
against the resolved default background to preserve behavior when different ids
resolve to the same actual color.

This preserved the actual GPU renderer path, fractional scaling, glyph/emoji
behavior, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, and cleared-background skip. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/background-id-runs/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.858985`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8423 | 1.6479 | 1.8511 | 1.013x | 0.984x | 1.000x |
| repaint | 0.8202 | 1.2399 | 1.8315 | 0.979x | 1.018x | 1.001x |
| scroll_ascii | 0.9942 | 0.9821 | 1.8646 | 1.002x | 0.987x | 1.000x |
| scroll_unicode | 0.9386 | 1.0690 | 1.8292 | 0.987x | 1.006x | 1.001x |
| scroll_emoji | 1.1012 | 0.8070 | 1.8686 | 0.961x | 1.043x | 1.000x |

## Decision

Rejected and reverted.

The id comparison helped cursor and ASCII wall slightly, but it badly regressed
repaint and emoji and lowered the weighted score. Returning/tracking the extra id
also made the color-resolution code more complex. Keep the simpler float color
comparison in the accepted state.

# Failed experiment: mark full-clear damage once

## Hypothesis

When the GPU path performs a full-frame clear, `xstartdraw()` calls `tfulldirt()`
and the renderer redraws every row. `gpudrawline()` then marks each row in the
current damage history one at a time. Since a full-clear/full-redraw frame dirties
the whole frame for future buffer-age repair, marking the entire current damage
array once at clear time and skipping per-row damage writes might reduce per-row
CPU work.

## Patch summary

In `x.c`, after full-frame `glClear()`, the experiment added:

```c
if (gpu.damage[0])
    memset(gpu.damage[gpu.damageidx], 1,
           gpu.damagerows * sizeof(*gpu.damage[gpu.damageidx]));
```

In `render/gpu.c`, `gpudrawline()` skipped its per-row damage write when
`gpu.clearedframe` was already true.

This preserved the actual GPU renderer path, the full-clear/default-background
skip behavior, alpha test, solid no-blend behavior, triangle batches, fractional
scaling, glyph atlas rendering, color emoji path, and conservative damage history
semantics. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/full-clear-damage-once/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.859671`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8281 | 1.6820 | 1.8487 | 0.986x | 1.016x | 0.999x |
| repaint | 0.8363 | 1.2185 | 1.8322 | 1.006x | 0.990x | 1.000x |
| scroll_ascii | 0.9863 | 1.0007 | 1.8660 | 1.008x | 1.004x | 1.000x |
| scroll_unicode | 0.9362 | 1.0759 | 1.8305 | 1.000x | 1.004x | 1.001x |
| scroll_emoji | 1.1262 | 0.7844 | 1.8685 | 0.984x | 1.028x | 0.999x |

## Decision

Rejected and reverted.

Although repaint and ASCII wall ratios improved slightly, the weighted score fell
and the highest-weight cursor workload regressed. The existing per-row damage
write remains faster overall in this benchmark state.

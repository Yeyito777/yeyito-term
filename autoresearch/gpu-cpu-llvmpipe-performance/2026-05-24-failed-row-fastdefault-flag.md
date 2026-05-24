# Failed experiment: hoist default-background fast-path row predicate

## Hypothesis

`gpudrawline()` checks `!IS_SET(MODE_REVERSE)`, `!debug_mode`, and `y != vimline`
inside the per-cell default-background fast path. These row-wide predicates do
not change within the row. Hoisting them into a single `fastdefault` boolean might
reduce per-cell branch work in the hot renderer loop.

## Patch summary

In `render/gpu.c`, changed `gpudrawline()` from checking row-wide conditions in
the per-cell fast path:

```c
if (g.bg == defaultbg && ... && !IS_SET(MODE_REVERSE) && !debug_mode && y != vimline)
```

to computing once per row:

```c
int fastdefault = !IS_SET(MODE_REVERSE) && !debug_mode && y != vimline;
```

and using `fastdefault` in the per-cell condition.

The actual GPU renderer path, accepted cleared-background skip, alpha test, solid
no-blend behavior, triangle batches, fractional scaling, glyph atlas rendering,
and color emoji path were otherwise unchanged. This did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/row-fastdefault-flag/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.866272`  
Relative score: `1.003x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8478 | 1.6322 | 1.8488 | 1.009x | 0.986x | 0.999x |
| repaint | 0.8187 | 1.2050 | 1.8326 | 0.985x | 0.979x | 1.000x |
| scroll_ascii | 0.9902 | 0.9731 | 1.8640 | 1.012x | 0.976x | 0.999x |
| scroll_unicode | 0.9434 | 1.0704 | 1.8317 | 1.008x | 0.999x | 1.002x |
| scroll_emoji | 1.1345 | 0.7720 | 1.8698 | 0.991x | 1.012x | 1.000x |

## Decision

Rejected and reverted.

Although the weighted score improved slightly and cursor improved, repaint wall
time regressed by about 1.5%. The acceptance rule does not allow material
regressions in the high-priority repaint workload, so the original direct
condition is kept.

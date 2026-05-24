# Failed experiment: inline cleared empty-row check

## Hypothesis

The accepted cleared-empty-row fast path calls `tlinelen(y)`. Inlining the small
`tlinelen()` logic inside `gpudrawline()` might avoid a function call and reuse the
known dirty span (`x1` / `x2`) while preserving the wrap check.

## Patch summary

In `render/gpu.c`, replaced the accepted:

```c
tlinelen(y) == 0
```

with an inline scan of `line[last - 1]` down to `x1`, including a check for
`ATTR_WRAP` at the right edge. The change still only applied on fully-cleared
frames when selection/search/reverse/debug/vimnav effects were inactive.

The actual GPU renderer path, accepted full-clear background skip, accepted
empty-row skip semantics, alpha test, solid no-blend behavior, triangle batches,
fractional scaling, glyph atlas rendering, and color emoji path were otherwise
unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/inline-empty-row-check/result.json`

## Result versus accepted cleared-empty-row state

Accepted score: `0.875049`  
Experiment score: `0.863225`  
Relative score: `0.986x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8362 | 1.6034 | 1.8517 | 0.991x | 0.964x | 1.002x |
| repaint | 0.8251 | 1.2357 | 1.8321 | 0.991x | 1.009x | 0.999x |
| scroll_ascii | 0.9859 | 0.9817 | 1.8647 | 1.003x | 0.998x | 1.000x |
| scroll_unicode | 0.9252 | 1.0931 | 1.8280 | 0.983x | 1.027x | 0.999x |
| scroll_emoji | 1.1520 | 0.7631 | 1.8683 | 0.931x | 1.076x | 0.999x |

## Decision

Rejected and reverted.

The inline scan regressed the weighted score and the high-priority cursor/repaint
wall ratios. The existing `tlinelen()` helper is clearer and benchmarks better.

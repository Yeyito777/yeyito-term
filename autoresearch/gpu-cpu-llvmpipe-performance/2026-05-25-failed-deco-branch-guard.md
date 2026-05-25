# Failed experiment: combine underline/strikethrough branch checks

## Hypothesis

Most benchmark cells do not use underline or strikethrough decoration. The GPU row
renderer still tests both decoration bits separately on every cell, and each
active decoration recomputes the cell width. Wrapping the two tests in a single
combined decoration check could reduce common-case branch work and avoid duplicate
width calculation for decorated cells.

## Patch summary

In `render/gpu.c`, the experiment changed both `gpudrawline()` and `gpudrawcell()`
from independent checks like:

```c
if (g.mode & ATTR_UNDERLINE) ... gpucellright(x, 0) ...;
if (g.mode & ATTR_STRUCK) ... gpucellright(x, 0) ...;
```

to:

```c
if (g.mode & (ATTR_UNDERLINE|ATTR_STRUCK)) {
    int decow = gpucellright(x, 0) - cellx;
    if (g.mode & ATTR_UNDERLINE) ...;
    if (g.mode & ATTR_STRUCK) ...;
}
```

Decoration behavior and draw ordering were unchanged.

This preserved the actual GPU renderer path, fractional scaling, glyph/emoji
rendering, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/deco-branch-guard/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.856612`  
Relative score: `0.984x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8347 | 1.6659 | 1.8469 | 0.994x | 1.017x | 0.997x |
| repaint | 0.8149 | 1.2594 | 1.8302 | 0.963x | 1.049x | 1.000x |
| scroll_ascii | 0.9851 | 0.9956 | 1.8658 | 1.000x | 1.010x | 1.000x |
| scroll_unicode | 0.9396 | 1.0687 | 1.8290 | 0.987x | 1.024x | 1.000x |
| scroll_emoji | 1.1344 | 0.7781 | 1.8700 | 1.000x | 1.008x | 0.999x |

## Decision

Rejected and reverted.

The extra combined branch did not help the common case and materially regressed
cursor/repaint wall time. The existing two direct decoration checks remain better
for this benchmark state.

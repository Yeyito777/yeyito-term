# Failed experiment: premark cursor overlay cells

## Hypothesis

`gpudrawcursor()` already computes whether selection/search are active and applies
selection/search marks to the old cursor glyph before restoring that cell. It then
calls `gpudrawcell()`, which checks `selection_active()` / `search_active()` again
and repeats the same mark tests. Avoiding those duplicate checks for cursor
overlay cells could reduce cursor-update overhead while preserving rendering.

## Patch summary

In `render/gpu.c`, the experiment split `gpudrawcell()` into a marked helper:

```c
static void gpudrawcellmarked(Glyph g, int x, int y, int overlay, int domarks);
```

The public `gpudrawcell()` wrapper passed `domarks = 1` for normal callers. When
`domarks` was false, the helper skipped internal selection/search-active checks
and used the already-marked glyph mode.

`gpudrawcursor()` continued to precompute `selactive` / `searchactive`, explicitly
applied selected/match bits to the old cursor glyph, and then restored it with:

```c
gpudrawcellmarked(og, ox, oy, 1, 0);
```

For focused block cursor cells, it similarly applied selected/match bits to the
cursor glyph before calling `gpudrawcellmarked(..., 0)`. Other cursor shapes and
normal line rendering were unchanged.

This preserved the actual GPU renderer path, cursor rendering behavior,
fractional scaling, glyph/emoji rendering, triangle batches, alpha test, solid
no-blend behavior, accepted clear-color cache, and cleared-background skip. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cursor-premarked-cells/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.860574`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8370 | 1.6542 | 1.8482 | 1.007x | 0.987x | 0.999x |
| repaint | 0.8344 | 1.2159 | 1.8328 | 0.996x | 0.998x | 1.002x |
| scroll_ascii | 0.9533 | 0.9962 | 1.8670 | 0.961x | 1.001x | 1.001x |
| scroll_unicode | 0.9436 | 1.0582 | 1.8300 | 0.993x | 0.996x | 1.001x |
| scroll_emoji | 1.1288 | 0.7769 | 1.8694 | 0.985x | 1.004x | 1.001x |

## Decision

Rejected and reverted.

The change produced a small cursor wall-speedup improvement, but total score fell
below the accepted clear-color cache state. ASCII scrolling regressed sharply and
emoji/repaint also slipped, so the added helper/API complexity is not justified.

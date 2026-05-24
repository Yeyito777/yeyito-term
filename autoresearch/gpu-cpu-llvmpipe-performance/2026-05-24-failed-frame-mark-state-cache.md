# Failed experiment: cache selection/search active state per GPU frame

## Hypothesis

`gpudrawline()`, `gpudrawcell()`, and `gpudrawcursor()` repeatedly ask whether a
selection or search is active. These active flags are frame-global for rendering.
Caching them once at GPU batch reset time might reduce repeated state queries in
line and cursor rendering while preserving selected/search-matched cell behavior.

## Patch summary

The experiment added two fields to `Gpu`:

```c
int searchactive, selactive;
```

`gpubatchreset()` filled them once per frame:

```c
gpu.searchactive = search_active();
gpu.selactive = selection_active();
```

and the GPU line/cell/cursor paths used those cached values instead of calling
`search_active()` / `selection_active()` locally.

This preserved the actual GPU renderer path, selection/search match checks,
fractional scaling, text/emoji rendering, the accepted clear-color cache,
cleared-background skip, triangle batches, alpha test, and solid no-blend
behavior. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/frame-mark-state-cache/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.850948`  
Relative score: `0.984x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8396 | 1.7147 | 1.8479 | 1.010x | 1.024x | 0.999x |
| repaint | 0.7988 | 1.2817 | 1.8318 | 0.953x | 1.052x | 1.001x |
| scroll_ascii | 0.9870 | 0.9923 | 1.8676 | 0.995x | 0.998x | 1.002x |
| scroll_unicode | 0.9334 | 1.0762 | 1.8290 | 0.982x | 1.013x | 1.001x |
| scroll_emoji | 1.1279 | 0.7828 | 1.8682 | 0.985x | 1.012x | 1.000x |

## Decision

Rejected and reverted.

Despite slightly helping cursor, the cached state regressed repaint badly and
lowered the weighted score. The local active-state calls are not the bottleneck in
this benchmark state, and the original code remains clearer.

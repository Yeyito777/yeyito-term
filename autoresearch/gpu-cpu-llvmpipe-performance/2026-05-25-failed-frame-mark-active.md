# Failed experiment: cache selection/search active flags once per GPU frame

## Hypothesis

The GPU renderer asks `selection_active()` and `search_active()` once per row and
again for cursor overlay cells. These flags are frame-wide for normal draw passes,
so caching them in `xstartdraw()` could remove repeated function calls from hot GPU
line/cell/cursor rendering paths without changing selected/search-highlight
behavior.

## Patch summary

The experiment added `gpu.selactive` and `gpu.searchactive`, initialized them once
per GPU frame in `xstartdraw()` after `gpubatchreset()`:

```c
gpu.selactive = selection_active();
gpu.searchactive = search_active();
```

Then `gpudrawline()`, `gpudrawcell()`, and `gpudrawcursor()` used the cached flags
instead of calling the helper functions directly.

Actual GPU rendering, selected/search match application, fractional scaling,
glyph/emoji rendering, batching, accepted clear-color cache, cleared-background
skip, and accepted vimnav row guard were otherwise unchanged. The benchmark still
compared same-source Xft and GPU paths under llvmpipe; it did not fallback to Xft
or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/frame-mark-active/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.862997`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8412 | 1.6470 | 1.8506 | 1.002x | 1.005x | 0.999x |
| repaint | 0.8373 | 1.2532 | 1.8320 | 0.989x | 1.044x | 1.001x |
| scroll_ascii | 0.9714 | 0.9955 | 1.8664 | 0.986x | 1.010x | 1.001x |
| scroll_unicode | 0.9374 | 1.0608 | 1.8299 | 0.985x | 1.016x | 1.000x |
| scroll_emoji | 1.1372 | 0.7676 | 1.8676 | 1.003x | 0.995x | 0.998x |

## Decision

Rejected and reverted.

The cached flags slightly improved cursor and emoji wall ratios, but repaint,
ASCII, and unicode wall ratios regressed. The extra per-frame state is not
worthwhile for the weighted llvmpipe benchmark.

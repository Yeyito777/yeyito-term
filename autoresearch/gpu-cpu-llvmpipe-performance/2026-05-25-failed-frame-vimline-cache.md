# Failed experiment: cache vimnav current line once per GPU frame

## Hypothesis

The accepted vimnav current-line guard avoids calling `vimnav_curline_y()` in
inactive vimnav mode, but it still evaluates `vimnav.mode ? ... : -1` once per
rendered row. Caching the resolved vimnav highlight row once per GPU frame in
`xstartdraw()` could reduce per-row work while preserving active vimnav behavior.

## Patch summary

The experiment added `int vimline;` to `Gpu`, set once per frame in `xstartdraw()`:

```c
gpu.vimline = vimnav.mode ? vimnav_curline_y() : -1;
```

and changed `gpudrawline()` to compare against `gpu.vimline` instead of a local
per-row calculation.

This preserved the actual GPU renderer path, fractional scaling, glyph/emoji
rendering, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard
semantics. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/frame-vimline-cache/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.860316`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8362 | 1.6594 | 1.8510 | 0.996x | 1.013x | 1.000x |
| repaint | 0.8242 | 1.2496 | 1.8317 | 0.973x | 1.041x | 1.001x |
| scroll_ascii | 0.9889 | 0.9799 | 1.8650 | 1.003x | 0.994x | 1.000x |
| scroll_unicode | 0.9321 | 1.0885 | 1.8266 | 0.980x | 1.043x | 0.998x |
| scroll_emoji | 1.1380 | 0.7680 | 1.8678 | 1.004x | 0.995x | 0.998x |

## Decision

Rejected and reverted.

Caching the vim line once per frame helped ASCII/emoji slightly, but cursor,
repaint, and unicode regressed and the weighted score dropped. The accepted simple
per-row guarded expression remains better.

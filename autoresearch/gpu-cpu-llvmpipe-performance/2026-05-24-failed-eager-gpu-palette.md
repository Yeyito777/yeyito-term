# Failed experiment: eager GPU palette refresh

## Hypothesis

`gpucolor()` currently checks `gpupalvalid` on every indexed color lookup and lazily
fills the GPU float palette the first time it is needed after colors load/change.
Since color loading already happens through `xloadcols()` / `xsetcolorname()`,
refreshing `gpupal` eagerly there could remove the hot-path validity branch from
`gpucolor()`.

## Patch summary

In `render/gpu.c`, the experiment replaced `gpupalvalid` with:

```c
static void gpupalrefresh(void) { ... }
```

and made `gpucolor()` directly index `gpupal` after truecolor handling. In `x.c`,
`xloadcols()` and `xsetcolorname()` called `gpupalrefresh()` after loading/updating
colors instead of invalidating the palette.

This preserved the actual GPU renderer path, color semantics including dynamic
color changes, fractional scaling, glyph/emoji behavior, triangle batches, alpha
test, solid no-blend behavior, accepted clear-color cache, and cleared-background
skip. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/eager-gpu-palette/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.861892`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8394 | 1.6989 | 1.8487 | 1.009x | 1.014x | 0.999x |
| repaint | 0.8335 | 1.2158 | 1.8330 | 0.995x | 0.998x | 1.002x |
| scroll_ascii | 0.9762 | 0.9840 | 1.8649 | 0.984x | 0.989x | 1.000x |
| scroll_unicode | 0.9320 | 1.0764 | 1.8287 | 0.980x | 1.013x | 1.001x |
| scroll_emoji | 1.1388 | 0.7778 | 1.8694 | 0.994x | 1.005x | 1.001x |

## Decision

Rejected and reverted.

The branch removal improved cursor wall/CPU, but repaint and scrolling wall ratios
regressed and the weighted score fell below accepted. The lazy `gpupalvalid` check
is not the current bottleneck.

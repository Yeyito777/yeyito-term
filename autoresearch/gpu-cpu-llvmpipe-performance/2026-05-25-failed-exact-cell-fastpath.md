# Failed experiment: exact-scale cell coordinate fast path

## Hypothesis

In normal, unscaled windows, the fractional-scaling helpers often resolve to an
exact 1:1 mapping where GPU cell coordinates can be computed as
`borderpx + x * win.cw` / `borderpx + y * win.ch`. Detecting that exact case once
per resize could avoid repeated floating-point scale/divide/round work in hot
`gpucellx()` / `gpucelly()` calls while preserving fractional scaling for all
non-exact cases.

## Patch summary

In `render/gpu.c`, the experiment added `gpu.exactx` and `gpu.exacty`, updated in
`gpuresize()` with:

```c
gpu.exactx = win.tw > 0 && win.w - 2 * borderpx == win.tw;
gpu.exacty = win.th > 0 && win.h - 2 * borderpx == win.th;
```

and used integer coordinate computation only when the corresponding exact flag was
set:

```c
if (gpu.exactx)
    return borderpx + x * win.cw;
return borderpx + gpuround(x * gpucellw());
```

The non-exact path was unchanged, so fractional scaling behavior remained
preserved. Actual GPU rendering, glyph/emoji drawing, triangle batches, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard were
otherwise unchanged. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/exact-cell-fastpath/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.860944`  
Relative score: `0.989x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8349 | 1.6501 | 1.8483 | 0.995x | 1.007x | 0.998x |
| repaint | 0.8134 | 1.2573 | 1.8319 | 0.961x | 1.048x | 1.001x |
| scroll_ascii | 0.9641 | 1.0198 | 1.8653 | 0.978x | 1.034x | 1.000x |
| scroll_unicode | 0.9292 | 1.0737 | 1.8282 | 0.977x | 1.028x | 0.999x |
| scroll_emoji | 1.2086 | 0.7168 | 1.8689 | 1.066x | 0.929x | 0.999x |

## Decision

Rejected and reverted.

The exact-scale fast path significantly improved emoji wall time, but it hurt the
higher-priority cursor, repaint, ASCII, and unicode workloads and reduced the
weighted score. The added branch in the coordinate helpers is not worthwhile for
this benchmark state; keep the accepted fractional-scaling helper implementation.

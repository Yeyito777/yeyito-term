# Failed experiment: skip default overlay cell backgrounds after clear

## Hypothesis

The accepted full-frame clear optimization skips default-background runs in
`gpudrawline()`, but single-cell overlay drawing through `gpudrawcell()` still
batches a background rectangle even when that rectangle resolves to the default
background already provided by `glClear()`. Skipping default-background overlay
rectangles on cleared frames might reduce cursor/update fill work while
preserving text, decorations, cursor overlays, and non-default backgrounds.

## Patch summary

In `render/gpu.c`, the experiment changed `gpudrawcell()` to compute the default
background color and only batch the cell background when either the frame was not
cleared or the resolved cell background differed from the default background:

```c
gpucolor(defaultbg, dbg);
if (!gpu.clearedframe || !gpucoloreq(bg, dbg))
    gpubatchrect(bb, cellx, celly, cellw, cellh, bg);
```

This preserved the actual GPU renderer path, triangle batches, alpha test,
solid no-blend behavior, glyph/emoji drawing, and the accepted cleared-background
line path. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/overlay-default-bg-skip/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.859388`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8501 | 1.6116 | 1.8500 | 1.012x | 0.973x | 1.000x |
| repaint | 0.8163 | 1.2966 | 1.8326 | 0.982x | 1.054x | 1.000x |
| scroll_ascii | 0.9636 | 1.0166 | 1.8647 | 0.985x | 1.020x | 0.999x |
| scroll_unicode | 0.9535 | 1.0406 | 1.8304 | 1.019x | 0.971x | 1.001x |
| scroll_emoji | 1.1346 | 0.7761 | 1.8691 | 0.992x | 1.018x | 1.000x |

## Decision

Rejected and reverted.

Although cursor wall improved, the total weighted score fell and repaint wall
regressed materially. The extra per-overlay default color check is not worth it
in the accepted benchmark state.

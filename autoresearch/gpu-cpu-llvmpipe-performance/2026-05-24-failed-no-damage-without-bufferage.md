# Failed experiment: skip damage history without buffer age

## Hypothesis

When GLX buffer age is unavailable, the accepted double-buffered llvmpipe path
clears and redraws the full frame after each swap. In that case the per-row damage
history is not used to repair an aged back buffer. Avoiding damage-history
allocation, per-frame memset, and per-row damage marking when `GLX_EXT_buffer_age`
is absent might reduce CPU overhead without changing rendering behavior.

## Patch summary

In `render/gpu.c`, changed `gpudamageensure()` to return unless buffer age support
is available:

```c
if (!gpu.doublebuf || !gpu.bufferage || rows <= 0)
    return;
```

With no damage arrays allocated, the existing `gpudrawline()` damage write path
also becomes inactive because it is guarded by `gpu.damage[0]`.

This preserved the actual GPU renderer path, the accepted clear-color cache,
full-frame clear behavior, cleared-background skip, triangle batches, alpha test,
solid no-blend behavior, fractional scaling, and glyph/emoji rendering. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-damage-without-bufferage/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.859944`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8356 | 1.6342 | 1.8462 | 1.005x | 0.976x | 0.998x |
| repaint | 0.8307 | 1.2215 | 1.8318 | 0.991x | 1.003x | 1.001x |
| scroll_ascii | 0.9812 | 0.9901 | 1.8650 | 0.989x | 0.995x | 1.000x |
| scroll_unicode | 0.9287 | 1.0752 | 1.8299 | 0.977x | 1.012x | 1.001x |
| scroll_emoji | 1.1108 | 0.7860 | 1.8709 | 0.970x | 1.016x | 1.001x |

## Decision

Rejected and reverted.

The simplification helped cursor wall slightly, but it lowered the weighted score
and regressed repaint plus scrolling wall ratios. Keeping the existing damage
history bookkeeping is better for the accepted benchmark state.

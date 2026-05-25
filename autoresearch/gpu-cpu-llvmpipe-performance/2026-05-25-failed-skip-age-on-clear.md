# Failed experiment: skip buffer-age query when GPU clear is already required

## Hypothesis

`xstartdraw()` queried `GLX_BACK_BUFFER_AGE_EXT` before checking `gpu.needclear`.
When `gpu.needclear` is already true, the renderer will clear and rebuild a full
frame regardless of buffer age, so skipping that query for those frames could reduce
startup/resize/atlas-reset overhead while preserving buffer-age repair semantics on
normal frames.

## Patch summary

In `x.c`, the experiment changed the buffer-age query guard from:

```c
if (gpu.doublebuf && gpu.bufferage)
    glXQueryDrawable(..., &gpu.backage);
```

to:

```c
if (!gpu.needclear && gpu.doublebuf && gpu.bufferage)
    glXQueryDrawable(..., &gpu.backage);
```

`gpu.backage` is still initialized to zero, so the existing full-clear path remains
used when needed. Actual GPU rendering, dirty/full redraw behavior, damage-history
repair, fractional scaling, glyph/emoji rendering, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
The benchmark still compared same-source Xft and actual GPU paths under llvmpipe;
it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-age-on-clear/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.864207`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8521 | 1.6266 | 1.8500 | 1.015x | 0.993x | 0.999x |
| repaint | 0.8171 | 1.2301 | 1.8300 | 0.965x | 1.025x | 1.000x |
| scroll_ascii | 0.9792 | 1.0032 | 1.8652 | 0.994x | 1.017x | 1.000x |
| scroll_unicode | 0.9430 | 1.0750 | 1.8328 | 0.991x | 1.030x | 1.002x |
| scroll_emoji | 1.1392 | 0.7620 | 1.8691 | 1.005x | 0.988x | 0.999x |

## Decision

Rejected and reverted.

Skipping the query helped cursor and emoji wall ratios, but repaint regressed
materially and the weighted score fell below accepted. Keep querying buffer age
before the clear decision in the accepted path.

# Failed experiment: cache clear color by color id

## Hypothesis

After accepting GL clear-color state caching, the clear path still resolves the
clear color with `gpucolor()` before comparing it to the cached float value. Since
full-frame clears normally use either `defaultbg` or `defaultfg` depending on
reverse-video mode, comparing the color id first and only calling `gpucolor()`
when the id changes might avoid a little per-frame palette work.

## Patch summary

The experiment extended the accepted clear-color cache with:

```c
uint32_t clearid;
```

and changed the clear path to compare `clearid` before calling `gpucolor()`.
It also invalidated `gpu.clearvalid` in `xloadcols()` / `xsetcolorname()` when
palette colors are reloaded or changed.

This preserved the actual GPU renderer path, full-frame clear behavior,
fractional scaling, GPU text/emoji behavior, triangle batches, alpha test, solid
no-blend behavior, and the accepted cleared-background optimization. It did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/clearcolor-id-cache/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.856597`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8346 | 1.6409 | 1.8469 | 1.004x | 0.979x | 0.998x |
| repaint | 0.8169 | 1.2490 | 1.8330 | 0.975x | 1.025x | 1.002x |
| scroll_ascii | 0.9872 | 0.9879 | 1.8629 | 0.995x | 0.993x | 0.999x |
| scroll_unicode | 0.9366 | 1.0728 | 1.8305 | 0.985x | 1.009x | 1.002x |
| scroll_emoji | 1.1144 | 0.7910 | 1.8675 | 0.973x | 1.022x | 1.000x |

## Decision

Rejected and reverted.

Avoiding `gpucolor()` in the clear path did not help. The extra id state and
palette invalidation regressed the weighted score and especially repaint. The
accepted float clear-color cache is better and simpler.

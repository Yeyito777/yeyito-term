# Failed experiment: enlarge GPU glyph hash table to 16k slots

## Hypothesis

Unicode and emoji workloads look up many non-ASCII glyphs through the GPU glyph
hash table. Increasing `GPU_GLYPH_HASH` from 8192 to 16384 slots could reduce
linear-probe collisions for non-ASCII glyph lookup and improve scroll workloads,
at the cost of a small amount of extra memory/clear work.

## Patch summary

In `x.c`, changed:

```c
#define GPU_GLYPH_HASH 8192
```

to:

```c
#define GPU_GLYPH_HASH 16384
```

No renderer behavior, glyph rasterization, atlas sizing, batching, fractional
scaling, or GL state behavior was changed. The actual GPU renderer path, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard were
preserved. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/glyph-hash-16k/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.859069`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8261 | 1.6698 | 1.8496 | 0.984x | 1.019x | 0.999x |
| repaint | 0.8304 | 1.2294 | 1.8334 | 0.981x | 1.024x | 1.002x |
| scroll_ascii | 0.9789 | 0.9881 | 1.8639 | 0.993x | 1.002x | 0.999x |
| scroll_unicode | 0.9268 | 1.0772 | 1.8279 | 0.974x | 1.032x | 0.999x |
| scroll_emoji | 1.1442 | 0.7600 | 1.8685 | 1.009x | 0.985x | 0.999x |

## Decision

Rejected and reverted.

The larger hash improved emoji wall slightly, but cursor, repaint, and unicode
wall ratios regressed materially. The added memory/clear footprint and lower
weighted score are not acceptable.

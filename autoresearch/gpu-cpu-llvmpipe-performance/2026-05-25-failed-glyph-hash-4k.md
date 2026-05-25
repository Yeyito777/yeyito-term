# Failed experiment: shrink GPU glyph hash table to 4k slots

## Hypothesis

The GPU glyph hash table is cleared on atlas reset and stored in the global GPU
state. If the accepted 8192-slot table is larger than needed for benchmark glyph
sets, reducing it to 4096 slots could lower reset/cache footprint while preserving
lookup behavior via the existing linear probing fallback.

## Patch summary

In `x.c`, changed:

```c
#define GPU_GLYPH_HASH 8192
```

to:

```c
#define GPU_GLYPH_HASH 4096
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/glyph-hash-4k/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.862398`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8398 | 1.6628 | 1.8486 | 1.001x | 1.015x | 0.998x |
| repaint | 0.8295 | 1.2207 | 1.8317 | 0.980x | 1.017x | 1.001x |
| scroll_ascii | 0.9822 | 0.9981 | 1.8654 | 0.997x | 1.012x | 1.000x |
| scroll_unicode | 0.9383 | 1.0654 | 1.8294 | 0.986x | 1.020x | 1.000x |
| scroll_emoji | 1.1281 | 0.7695 | 1.8707 | 0.995x | 0.997x | 1.000x |

## Decision

Rejected and reverted.

Shrinking the hash did not improve RSS enough to matter and regressed high-priority
repaint and unicode wall ratios. Keep the accepted 8192-slot hash table.

# Failed experiment: replace floor-based positive rounding with cast

## Hypothesis

GPU coordinate and size rounding operates on non-negative terminal geometry,
font sizes, and glyph dimensions. Replacing `floor(v + 0.5)` with a simple
`(int)(v + 0.5)` could preserve results for these positive values while avoiding a
libm/helper path in hot geometry calculations.

## Patch summary

In `render/gpu.c`, changed:

```c
return (int)floor(v + 0.5);
```

to:

```c
return (int)(v + 0.5);
```

No renderer behavior was otherwise changed. Fractional scaling semantics for the
positive values used by the GPU renderer should be equivalent, and the actual GPU
renderer path, glyph/emoji rendering, triangle batches, alpha test, solid
no-blend behavior, accepted clear-color cache, cleared-background skip, and
accepted vimnav row guard were preserved. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/positive-round-cast/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.864479`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8468 | 1.6304 | 1.8483 | 1.009x | 0.995x | 0.998x |
| repaint | 0.8274 | 1.2367 | 1.8301 | 0.977x | 1.030x | 1.000x |
| scroll_ascii | 0.9910 | 0.9908 | 1.8682 | 1.006x | 1.005x | 1.002x |
| scroll_unicode | 0.9410 | 1.0681 | 1.8284 | 0.989x | 1.023x | 0.999x |
| scroll_emoji | 1.1214 | 0.7768 | 1.8682 | 0.989x | 1.007x | 0.998x |

## Decision

Rejected and reverted.

The cast-based round improved cursor and ASCII wall slightly, but it materially
regressed repaint and unicode, lowering the weighted score below the accepted
vimnav-guard state. Keep the original `floor()` formulation.

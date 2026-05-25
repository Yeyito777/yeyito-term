# Failed experiment: build with -ffast-math

## Hypothesis

The GPU renderer does frequent floating-point geometry/scaling calculations
(`floor`, `fabs`, divisions for cell/texture coordinates). Building with
`-ffast-math` might let the compiler simplify those operations and improve the
llvmpipe GPU path enough to beat the accepted state.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -ffast-math
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared the same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/fast-math/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.859318`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8352 | 1.6398 | 1.8493 | 0.995x | 1.001x | 0.999x |
| repaint | 0.8167 | 1.2433 | 1.8303 | 0.965x | 1.036x | 1.000x |
| scroll_ascii | 0.9854 | 0.9924 | 1.8668 | 1.000x | 1.006x | 1.001x |
| scroll_unicode | 0.9324 | 1.0714 | 1.8300 | 0.980x | 1.026x | 1.000x |
| scroll_emoji | 1.1429 | 0.7676 | 1.8681 | 1.008x | 0.995x | 0.998x |

## Decision

Rejected and reverted.

`-ffast-math` did not produce a stable improvement. Emoji wall increased slightly,
but repaint and unicode regressed substantially and the total score fell well below
accepted. Keep the accepted conservative floating-point build flags.

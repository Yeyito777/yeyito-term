# Failed experiment: build with -fno-signed-zeros

## Hypothesis

The GPU renderer's floating-point geometry uses positive pixel coordinates and
scales, so signed-zero semantics should not matter. Adding `-fno-signed-zeros`
without the broader `-ffast-math` bundle might allow slightly better floating-point
optimization while preserving renderer behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-signed-zeros
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-signed-zeros/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.858764`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8267 | 1.6813 | 1.8481 | 0.985x | 1.026x | 0.998x |
| repaint | 0.8296 | 1.2267 | 1.8295 | 0.980x | 1.022x | 1.000x |
| scroll_ascii | 0.9943 | 1.0208 | 1.8647 | 1.009x | 1.035x | 1.000x |
| scroll_unicode | 0.9362 | 1.0745 | 1.8304 | 0.984x | 1.029x | 1.000x |
| scroll_emoji | 1.1363 | 0.7753 | 1.8686 | 1.002x | 1.005x | 0.999x |

## Decision

Rejected and reverted.

The flag slightly helped ASCII and emoji wall ratios but regressed cursor, repaint,
and unicode enough to lower the weighted score. Keep the accepted floating-point
flags.

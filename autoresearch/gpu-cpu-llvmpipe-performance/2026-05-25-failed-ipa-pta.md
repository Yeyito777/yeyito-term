# Failed experiment: build with -fipa-pta

## Hypothesis

The renderer is included in `x.c` and the accepted build already uses LTO, but GCC's
interprocedural points-to analysis may still expose additional alias information
for the large single executable. Adding `-fipa-pta` could improve optimization of
hot GPU/Xft paths under the llvmpipe benchmark.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fipa-pta
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/ipa-pta/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.846512`  
Relative score: `0.973x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8369 | 2.4839 | 1.8360 | 0.997x | 1.516x | 0.991x |
| repaint | 0.8317 | 1.2192 | 1.8321 | 0.982x | 1.016x | 1.001x |
| scroll_ascii | 0.9860 | 0.9944 | 1.8654 | 1.000x | 1.009x | 1.000x |
| scroll_unicode | 0.9402 | 1.0879 | 1.8277 | 0.988x | 1.042x | 0.999x |
| scroll_emoji | 1.1448 | 0.7630 | 1.8703 | 1.010x | 0.989x | 0.999x |

## Decision

Rejected and reverted.

The flag helped emoji wall and left ASCII nearly flat, but it made cursor CPU much
worse and reduced the weighted score substantially. Keep the accepted build flags.

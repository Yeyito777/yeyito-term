# Failed experiment: build with -fno-unwind-tables

## Hypothesis

A previous `-fno-asynchronous-unwind-tables` build-flag experiment reduced some
unwind metadata but still landed below accepted. Adding `-fno-unwind-tables` alone
might reduce binary metadata/code-layout pressure differently, possibly improving
instruction-cache behavior in the CPU-bound llvmpipe renderer without changing
runtime semantics.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-unwind-tables
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-unwind-tables/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.861259`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8321 | 1.6673 | 1.8521 | 0.991x | 1.018x | 1.000x |
| repaint | 0.8323 | 1.2291 | 1.8318 | 0.983x | 1.024x | 1.001x |
| scroll_ascii | 0.9849 | 0.9932 | 1.8621 | 0.999x | 1.007x | 0.998x |
| scroll_unicode | 0.9349 | 1.0703 | 1.8265 | 0.982x | 1.025x | 0.998x |
| scroll_emoji | 1.1351 | 0.7671 | 1.8689 | 1.001x | 0.994x | 0.999x |

## Decision

Rejected and reverted.

The flag was behavior-preserving and kept emoji essentially flat, but cursor,
repaint, and unicode wall ratios regressed and the total score fell below accepted.
Keep the accepted build flags.

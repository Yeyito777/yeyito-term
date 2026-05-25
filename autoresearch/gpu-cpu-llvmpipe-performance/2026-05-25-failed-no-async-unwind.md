# Failed experiment: build without asynchronous unwind tables

## Hypothesis

Removing asynchronous unwind table generation can shrink the executable and may
improve i-cache behavior in hot renderer code. Adding
`-fno-asynchronous-unwind-tables` to the optimized build might therefore improve the
same-source llvmpipe GPU-vs-Xft benchmark without changing runtime behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-asynchronous-unwind-tables
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-async-unwind/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.865977`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8384 | 1.6477 | 1.8525 | 0.999x | 1.006x | 1.000x |
| repaint | 0.8398 | 1.2050 | 1.8306 | 0.992x | 1.004x | 1.000x |
| scroll_ascii | 0.9667 | 1.0147 | 1.8653 | 0.981x | 1.029x | 1.000x |
| scroll_unicode | 0.9374 | 1.0665 | 1.8274 | 0.985x | 1.022x | 0.999x |
| scroll_emoji | 1.1296 | 0.7102 | 1.8690 | 0.996x | 0.920x | 0.999x |

## Decision

Rejected and reverted.

The build flag did not improve the weighted score. It was near accepted on cursor
and repaint wall, but ASCII/unicode/emoji wall ratios regressed and the total score
fell below the accepted vimnav row guard.

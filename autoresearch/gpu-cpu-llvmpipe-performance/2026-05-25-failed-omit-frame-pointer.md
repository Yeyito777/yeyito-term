# Failed experiment: build with -fomit-frame-pointer

## Hypothesis

Although optimized GCC builds often omit frame pointers by default on x86-64,
adding `-fomit-frame-pointer` explicitly might free one register and reduce call/
prologue overhead in the hot renderer and llvmpipe paths enough to improve the
same-source GPU-vs-Xft benchmark.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fomit-frame-pointer
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/omit-frame-pointer/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.857448`  
Relative score: `0.985x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8421 | 1.6557 | 1.8515 | 1.003x | 1.011x | 1.000x |
| repaint | 0.8072 | 1.2969 | 1.8315 | 0.953x | 1.080x | 1.001x |
| scroll_ascii | 0.9929 | 0.9867 | 1.8642 | 1.008x | 1.001x | 0.999x |
| scroll_unicode | 0.9493 | 1.0689 | 1.8290 | 0.998x | 1.024x | 1.000x |
| scroll_emoji | 1.1356 | 0.7747 | 1.8689 | 1.002x | 1.004x | 0.999x |

## Decision

Rejected and reverted.

The flag slightly improved cursor, ASCII, and emoji wall ratios, but repaint
regressed badly and the weighted score fell well below accepted. Keep the accepted
build flags.

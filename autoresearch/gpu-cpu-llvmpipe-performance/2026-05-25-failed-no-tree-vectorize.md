# Failed experiment: build with -fno-tree-vectorize

## Hypothesis

Some hot loops in the terminal renderer and same-source Xft comparison are small
or branch-heavy enough that GCC's tree vectorizer might add code size or setup
cost without much benefit. Disabling tree vectorization could improve instruction
cache/locality and reduce overhead in the llvmpipe benchmark.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-tree-vectorize
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-tree-vectorize/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.865271`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8378 | 1.6680 | 1.8477 | 0.998x | 1.018x | 0.998x |
| repaint | 0.8402 | 1.2165 | 1.8299 | 0.992x | 1.014x | 1.000x |
| scroll_ascii | 0.9723 | 0.9942 | 1.8683 | 0.987x | 1.008x | 1.002x |
| scroll_unicode | 0.9520 | 1.0737 | 1.8291 | 1.000x | 1.028x | 1.000x |
| scroll_emoji | 1.1437 | 0.7675 | 1.8693 | 1.009x | 0.995x | 0.999x |

## Decision

Rejected and reverted.

Disabling tree vectorization helped unicode and emoji wall ratios slightly, but it
regressed cursor, repaint, and ASCII enough to lower the weighted score. Keep the
accepted vectorization behavior from `-O3 -march=native -flto`.

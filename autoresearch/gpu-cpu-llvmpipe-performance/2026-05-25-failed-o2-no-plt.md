# Failed experiment: build with -O2 and -fno-plt

## Hypothesis

Separate build-flag experiments with `-O2` and `-fno-plt` each came close to the
accepted score but did not validate as wins. Combining the smaller `-O2` code shape
with direct external calls from `-fno-plt` might improve instruction/cache behavior
and shared-library call overhead enough to produce a reproducible llvmpipe gain.

## Patch summary

In `config.mk`, changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O2 -march=native -flto -fno-plt
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared the same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/o2-no-plt/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/o2-no-plt-validate/result.json`

## Validation result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment validation score: `0.859720`  
Relative score: `0.988x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8402 | 1.6772 | 1.8512 | 1.001x | 1.024x | 1.000x |
| repaint | 0.8216 | 1.2392 | 1.8305 | 0.970x | 1.032x | 1.000x |
| scroll_ascii | 0.9889 | 0.9909 | 1.8643 | 1.003x | 1.005x | 1.000x |
| scroll_unicode | 0.9225 | 1.0860 | 1.8281 | 0.969x | 1.040x | 0.999x |
| scroll_emoji | 1.1427 | 0.7738 | 1.8689 | 1.008x | 1.003x | 0.999x |

## Decision

Rejected and reverted.

The initial 7-iteration result was positive (`0.873578`, `1.0038x` accepted), but
the 9-iteration validation did not reproduce the gain. Validation regressed repaint
and unicode enough to land well below accepted. Keep the accepted `-O3` build flags.

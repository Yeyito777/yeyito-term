# Failed experiment: build with -fno-plt

## Hypothesis

The GPU renderer makes many calls into GL/GLX/Freetype/Xft-related shared-library
functions. Building with `-fno-plt` can avoid Procedure Linkage Table stubs for
external calls on ELF targets, possibly reducing CPU overhead in the software-GL
renderer path without changing source-level behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-plt
```

No renderer code, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared the same-source Xft build against the
actual GPU renderer under llvmpipe; it did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-plt/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-plt-validate/result.json`

## Validation result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment validation score: `0.869960`  
Relative score: `0.9997x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8305 | 1.6757 | 1.8479 | 0.990x | 1.023x | 0.998x |
| repaint | 0.8468 | 1.1927 | 1.8311 | 1.000x | 0.994x | 1.001x |
| scroll_ascii | 0.9995 | 0.9762 | 1.8655 | 1.014x | 0.990x | 1.000x |
| scroll_unicode | 0.9477 | 1.0493 | 1.8292 | 0.996x | 1.005x | 1.000x |
| scroll_emoji | 1.1452 | 0.7625 | 1.8718 | 1.010x | 0.988x | 1.000x |

## Decision

Rejected and reverted.

The first run was promising (`0.878457`, about `1.009x` accepted) with improvements
in cursor, repaint, ASCII, and unicode wall ratios. The 9-iteration validation did
not reproduce that gain: the total score landed just below accepted and the
high-priority cursor workload regressed. Since this is a broad build-flag change,
it needs a clear and reproducible win; keep the accepted flags.

# Failed experiment: build with -fno-math-errno

## Hypothesis

The GPU renderer uses libm-style operations such as `floor()` and `fabs()` in hot
geometry paths. The program does not inspect `errno` from math calls. Adding
`-fno-math-errno` to the optimized build flags might let the compiler inline or
simplify math operations and reduce GPU geometry overhead, while preserving normal
C behavior the program relies on.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-math-errno
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

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-math-errno/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868343`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8499 | 1.6211 | 1.8518 | 1.013x | 0.990x | 1.000x |
| repaint | 0.8354 | 1.2178 | 1.8320 | 0.987x | 1.015x | 1.001x |
| scroll_ascii | 0.9797 | 0.9873 | 1.8655 | 0.994x | 1.001x | 1.000x |
| scroll_unicode | 0.9403 | 1.0655 | 1.8289 | 0.988x | 1.021x | 1.000x |
| scroll_emoji | 1.1364 | 0.7724 | 1.8683 | 1.002x | 1.001x | 0.998x |

## Decision

Rejected and reverted.

The flag produced a nice cursor-wall improvement and was close overall, but the
weighted score remained below the accepted vimnav-guard state and repaint wall
regressed. Since this is a broad build-flag change rather than a focused renderer
improvement, it is not worth accepting without a clear score win.

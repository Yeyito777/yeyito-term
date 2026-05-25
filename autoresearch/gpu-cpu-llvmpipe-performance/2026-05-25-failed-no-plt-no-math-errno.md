# Failed experiment: build with -fno-plt and -fno-math-errno

## Hypothesis

The separate `-fno-plt` and `-fno-math-errno` build-flag experiments each had
some promising workload movements but failed acceptance. Combining them might keep
`-fno-plt`'s lower shared-library call overhead while also letting the compiler
simplify math calls in GPU geometry paths, producing a reproducible gain for the
software-GL renderer.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-plt -fno-math-errno
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-plt-no-math-errno/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.870515`  
Relative score: `1.0003x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8300 | 1.6585 | 1.8508 | 0.989x | 1.012x | 0.999x |
| repaint | 0.8520 | 1.1910 | 1.8301 | 1.006x | 0.992x | 1.000x |
| scroll_ascii | 0.9981 | 0.9771 | 1.8650 | 1.013x | 0.991x | 1.000x |
| scroll_unicode | 0.9496 | 1.0442 | 1.8286 | 0.998x | 1.000x | 0.999x |
| scroll_emoji | 1.1313 | 0.7662 | 1.8705 | 0.998x | 0.993x | 1.000x |

## Decision

Rejected and reverted.

The weighted score was technically just above accepted, but only by `0.03%`, which
is well within run noise for this benchmark. More importantly, the highest-weight
cursor workload regressed by about `1.1%` in wall ratio, violating the acceptance
rule for high-priority workloads. Because this is also a broad build-flag change
rather than a focused renderer improvement, it is not worth keeping without a
clear reproducible margin and no cursor/repaint regression.

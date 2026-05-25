# Failed experiment: build with -fno-stack-protector

## Hypothesis

If the system compiler injects stack canaries by default for some functions, adding
`-fno-stack-protector` could remove canary loads/stores/checks from hot renderer or
benchmark-adjacent paths, improving the CPU-bound llvmpipe comparison without
changing renderer behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-stack-protector
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-stack-protector/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868234`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8400 | 1.6520 | 1.8487 | 1.001x | 1.009x | 0.998x |
| repaint | 0.8417 | 1.2008 | 1.8296 | 0.994x | 1.000x | 1.000x |
| scroll_ascii | 0.9896 | 0.9773 | 1.8629 | 1.004x | 0.991x | 0.999x |
| scroll_unicode | 0.9363 | 1.0723 | 1.8320 | 0.984x | 1.027x | 1.001x |
| scroll_emoji | 1.1385 | 0.7731 | 1.8684 | 1.004x | 1.002x | 0.998x |

## Decision

Rejected and reverted.

The flag was close and helped cursor/ASCII/emoji wall ratios slightly, but repaint
and unicode wall ratios regressed enough to keep the total score below accepted.
Keep the accepted build flags.

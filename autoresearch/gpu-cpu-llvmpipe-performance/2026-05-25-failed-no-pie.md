# Failed experiment: build non-PIE with -fno-pie/-no-pie

## Hypothesis

The toolchain may default to position-independent executables. Building `st` as a
non-PIE executable with `-fno-pie` / `-no-pie` could reduce GOT/relocation overhead
in the CPU-heavy same-source Xft and llvmpipe GPU binaries, possibly improving the
weighted GPU-vs-Xft score without touching renderer behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
LDFLAGS = -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-pie
LDFLAGS = -flto -no-pie
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-pie/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-pie-validate/result.json`

## Validation result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment validation score: `0.868077`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8343 | 1.6562 | 1.8533 | 0.994x | 1.011x | 1.001x |
| repaint | 0.8346 | 1.1896 | 1.8304 | 0.986x | 0.991x | 1.000x |
| scroll_ascii | 0.9988 | 0.9721 | 1.8645 | 1.014x | 0.986x | 1.000x |
| scroll_unicode | 0.9431 | 1.0663 | 1.8277 | 0.991x | 1.021x | 0.999x |
| scroll_emoji | 1.1446 | 0.7709 | 1.8697 | 1.009x | 0.999x | 0.999x |

## Decision

Rejected and reverted.

The first 7-iteration run was promising (`0.875377`, about `1.006x` accepted), but
validation did not reproduce the gain. The validated score was below accepted and
high-weight cursor/repaint wall ratios regressed. Keep the accepted PIE/default
build flags.

# Failed experiment: build with -fcf-protection=none

## Hypothesis

If the host toolchain enables Intel CET/control-flow protection by default, explicit
`-fcf-protection=none` could remove indirect-branch/check overhead and improve the
CPU-bound llvmpipe renderer. If the default is already none, this should be a
behavior-preserving no-op aside from possible code-layout changes.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fcf-protection=none
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-cf-protection/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868124`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8373 | 1.6212 | 1.8494 | 0.998x | 0.990x | 0.999x |
| repaint | 0.8446 | 1.2106 | 1.8314 | 0.998x | 1.009x | 1.001x |
| scroll_ascii | 0.9857 | 0.9925 | 1.8652 | 1.000x | 1.007x | 1.000x |
| scroll_unicode | 0.9364 | 1.0810 | 1.8289 | 0.984x | 1.035x | 1.000x |
| scroll_emoji | 1.1490 | 0.7757 | 1.8698 | 1.013x | 1.005x | 0.999x |

## Decision

Rejected and reverted.

The flag improved emoji wall and left several workloads close to accepted, but the
total score remained below accepted and unicode/cursor/repaint were not clear wins.
Keep the accepted build flags.

# Failed experiment: build with -fno-stack-clash-protection

## Hypothesis

If the host compiler enables stack-clash probing by default, adding
`-fno-stack-clash-protection` could remove stack probe overhead or code-layout
noise in the CPU-bound llvmpipe/Xft benchmark while preserving renderer behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-stack-clash-protection
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-stack-clash/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.854786`  
Relative score: `0.982x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8278 | 1.6809 | 1.8515 | 0.986x | 1.026x | 1.000x |
| repaint | 0.8237 | 1.2389 | 1.8313 | 0.973x | 1.032x | 1.001x |
| scroll_ascii | 0.9803 | 0.9933 | 1.8643 | 0.995x | 1.007x | 0.999x |
| scroll_unicode | 0.9348 | 1.0653 | 1.8311 | 0.982x | 1.020x | 1.001x |
| scroll_emoji | 1.1105 | 0.7803 | 1.8709 | 0.979x | 1.011x | 1.000x |

## Decision

Rejected and reverted.

The flag regressed all workload wall ratios relative to accepted and lowered the
weighted score substantially. Keep the accepted build flags.

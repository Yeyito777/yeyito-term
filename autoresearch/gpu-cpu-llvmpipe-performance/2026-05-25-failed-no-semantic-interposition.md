# Failed experiment: build with -fno-semantic-interposition

## Hypothesis

The accepted build uses `-O3 -march=native -flto`. Adding
`-fno-semantic-interposition` might allow slightly more aggressive optimization of
non-overridable function calls and globals in this executable, reducing CPU overhead
in the software-GL renderer and same-source Xft comparison build.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-semantic-interposition
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-semantic-interposition/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868620`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8396 | 1.6408 | 1.8491 | 1.000x | 1.002x | 0.999x |
| repaint | 0.8219 | 1.2520 | 1.8322 | 0.971x | 1.043x | 1.001x |
| scroll_ascii | 0.9666 | 1.0039 | 1.8669 | 0.981x | 1.018x | 1.001x |
| scroll_unicode | 0.9290 | 1.0828 | 1.8289 | 0.976x | 1.037x | 1.000x |
| scroll_emoji | 1.2460 | 0.6976 | 1.8709 | 1.099x | 0.904x | 1.000x |

## Decision

Rejected and reverted.

The flag helped emoji wall substantially in this run, but it regressed repaint,
ASCII, and unicode wall ratios and slightly lowered the weighted score. As a broad
build-flag change, it needs a clear and reproducible total-score win; keep the
accepted flags.

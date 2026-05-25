# Failed experiment: align loops to 16 bytes

## Hypothesis

The earlier 32-byte loop alignment was close but too large for some workloads. A
smaller 16-byte loop alignment could improve hot-loop fetch/branch behavior with
less code-size pressure.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -falign-loops=16
```

No renderer fallback, `gpudraw` bypass, renderer switch, llvmpipe detection trick,
text/emoji change, fractional scaling change, batching semantic change, clear-color
cache change, cleared-background skip change, or accepted vimnav row guard change
was used.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/align-loops-16/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.861835`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8384 | 1.6725 | 1.8508 | 0.999x | 1.021x | 0.999x |
| repaint | 0.8242 | 1.2308 | 1.8303 | 0.974x | 1.025x | 1.000x |
| scroll_ascii | 0.9873 | 0.9976 | 1.8644 | 1.002x | 1.012x | 1.000x |
| scroll_unicode | 0.9405 | 1.0521 | 1.8307 | 0.988x | 1.008x | 1.001x |
| scroll_emoji | 1.1396 | 0.7718 | 1.8687 | 1.005x | 1.000x | 0.999x |

## Decision

Rejected and reverted.

The 16-byte alignment helped ASCII/emoji wall ratios slightly, but repaint and
unicode regressed enough to drop the total score well below accepted. Keep the
accepted build flags.

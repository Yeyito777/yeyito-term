# Failed experiment: add -fno-jump-tables

## Hypothesis

The renderer and terminal hot path are branch-heavy with only a few small switches.
Disabling compiler jump tables could reduce indirect branch overhead or improve code
layout enough to help the llvmpipe GPU benchmark while keeping the actual renderer
unchanged.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fno-jump-tables
```

No renderer fallback, `gpudraw` bypass, llvmpipe detection, batching behavior, text,
emoji, fractional scaling, clear-color cache, cleared-background skip, or accepted
vimnav row guard behavior was changed.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-jump-tables/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.862532`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8316 | 1.6829 | 1.8490 | 0.991x | 1.027x | 0.999x |
| repaint | 0.8278 | 1.2139 | 1.8304 | 0.978x | 1.011x | 1.000x |
| scroll_ascii | 0.9862 | 0.9678 | 1.8635 | 1.001x | 0.982x | 0.999x |
| scroll_unicode | 0.9508 | 1.0624 | 1.8286 | 0.999x | 1.018x | 0.999x |
| scroll_emoji | 1.1420 | 0.7873 | 1.8663 | 1.007x | 1.020x | 0.997x |

## Decision

Rejected and reverted.

The flag helped ASCII and emoji wall ratios slightly, but cursor and repaint wall
speedups regressed and the weighted score remained below the accepted frontier.
Keep the accepted build flags.

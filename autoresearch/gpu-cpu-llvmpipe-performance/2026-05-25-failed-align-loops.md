# Failed experiment: align loops to 32 bytes

## Hypothesis

The GPU renderer hot path is dominated by tight per-row/per-cell loops and llvmpipe
client-array traversal. Aligning loop entries to 32 bytes might improve instruction
fetch and branch prediction enough to help the software-GL benchmark.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -falign-loops=32
```

The actual GPU renderer remained active. No Xft fallback, `gpudraw` bypass,
renderer switch, llvmpipe detection trick, batching semantic change, text/emoji
change, fractional scaling change, clear-color cache change, cleared-background
skip change, or accepted vimnav row guard change was used.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/align-loops/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868982`  
Relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8366 | 1.6845 | 1.8473 | 0.997x | 1.028x | 0.998x |
| repaint | 0.8477 | 1.1904 | 1.8300 | 1.001x | 0.992x | 1.000x |
| scroll_ascii | 0.9816 | 0.9853 | 1.8634 | 0.996x | 0.999x | 0.999x |
| scroll_unicode | 0.9453 | 1.0902 | 1.8287 | 0.993x | 1.044x | 0.999x |
| scroll_emoji | 1.1528 | 0.7576 | 1.8694 | 1.017x | 0.982x | 0.999x |

## Decision

Rejected and reverted.

Loop alignment was close and improved repaint/emoji wall ratios, but cursor, ASCII,
and unicode wall ratios remained below accepted. The weighted score did not beat
the accepted frontier, so keep the accepted build flags.

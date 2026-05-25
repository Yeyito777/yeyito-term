# Failed experiment: compare GPU colors with memcmp

## Hypothesis

`gpucoloreq()` is used frequently for background run merging and clear-color state.
Replacing three scalar float comparisons with a single `memcmp()` over the three
float values might compile into a tighter byte/word comparison and reduce hot-path
branch work.

## Patch summary

In `render/gpu.c`, the experiment changed:

```c
return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
```

to:

```c
return memcmp(a, b, 3 * sizeof *a) == 0;
```

No renderer fallback, `gpudraw` bypass, renderer switch, llvmpipe detection trick,
text/emoji change, fractional scaling change, batching semantic change, clear-color
cache removal, cleared-background skip change, or accepted vimnav row guard change
was used.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/color-memcmp/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.861904`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8453 | 1.6267 | 1.8466 | 1.007x | 0.993x | 0.997x |
| repaint | 0.8341 | 1.2356 | 1.8283 | 0.985x | 1.029x | 0.999x |
| scroll_ascii | 0.9549 | 1.0158 | 1.8631 | 0.969x | 1.030x | 0.999x |
| scroll_unicode | 0.9425 | 1.0761 | 1.8296 | 0.990x | 1.031x | 1.000x |
| scroll_emoji | 1.1304 | 0.7683 | 1.8682 | 0.997x | 0.996x | 0.998x |

## Decision

Rejected and reverted.

The bytewise compare improved cursor wall but regressed repaint, ASCII, unicode,
and emoji wall ratios. The total score stayed below accepted. Keep the scalar float
comparisons.

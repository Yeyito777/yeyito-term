# Failed experiment: fast reject non-emoji runes

## Hypothesis

Most glyph lookups in ASCII/unicode workloads are not emoji. `gpuisemoji()` checks
both the large emoji block and the lower symbol block for every glyph lookup. A
cheap `r < 0x2600` fast reject could slightly reduce per-glyph CPU work for the
common ASCII/Greek/CJK paths without changing classification.

## Patch summary

In `render/gpu.c`, changed `gpuisemoji()` from only range checks to:

```c
if (r < 0x2600)
    return 0;
return BETWEEN(r, 0x1f000, 0x1faff) || BETWEEN(r, 0x2600, 0x27bf);
```

The actual GPU renderer path, emoji classification for all accepted ranges,
fractional scaling, glyph/emoji rendering, triangle batches, alpha test, solid
no-blend behavior, accepted clear-color cache, and cleared-background skip were
otherwise preserved. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/emoji-fastreject/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.861312`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8342 | 1.6246 | 1.8500 | 1.003x | 0.970x | 1.000x |
| repaint | 0.8288 | 1.2354 | 1.8297 | 0.989x | 1.014x | 1.000x |
| scroll_ascii | 0.9732 | 1.0047 | 1.8645 | 0.981x | 1.010x | 1.000x |
| scroll_unicode | 0.9452 | 1.0529 | 1.8295 | 0.994x | 0.991x | 1.001x |
| scroll_emoji | 1.1357 | 0.7739 | 1.8679 | 0.991x | 1.000x | 1.000x |

## Decision

Rejected and reverted.

The branch saved no useful work in practice. Cursor wall improved slightly, but
repaint and ASCII scrolling regressed and the weighted score fell below accepted.
The original compact range expression remains preferable.

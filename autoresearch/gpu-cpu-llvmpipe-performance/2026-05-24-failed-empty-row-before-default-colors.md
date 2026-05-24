# Failed experiment: check cleared empty rows before default color resolution

## Hypothesis

After accepting the cleared-empty-row fast path, `gpudrawline()` still resolves
default foreground/background colors before checking whether a fully-cleared row
is empty. Moving the empty-row early return before `gpucolor(defaultfg, ...)` and
`gpucolor(defaultbg, ...)` might avoid two palette lookups for empty rows.

## Patch summary

In `render/gpu.c`, moved the accepted empty-row test above the default color
resolution in `gpudrawline()`:

```c
if (gpu.clearedframe && ... && tlinelen(y) == 0)
    return;
gpucolor(defaultfg, dfg);
gpucolor(defaultbg, dbg);
```

The actual GPU renderer path, accepted full-clear background skip, accepted
empty-row skip, alpha test, solid no-blend behavior, triangle batches, fractional
scaling, glyph atlas rendering, and color emoji path were otherwise unchanged.
This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-empty-before-default-colors/result.json`

## Result versus accepted cleared-empty-row state

Accepted score: `0.875049`  
Experiment score: `0.861332`  
Relative score: `0.984x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8432 | 1.6655 | 1.8490 | 0.999x | 1.001x | 1.001x |
| repaint | 0.8268 | 1.2384 | 1.8322 | 0.993x | 1.011x | 0.999x |
| scroll_ascii | 0.9669 | 0.9824 | 1.8648 | 0.984x | 0.998x | 1.000x |
| scroll_unicode | 0.9346 | 1.0804 | 1.8305 | 0.993x | 1.015x | 1.001x |
| scroll_emoji | 1.1432 | 0.7657 | 1.8684 | 0.924x | 1.080x | 1.000x |

## Decision

Rejected and reverted.

Despite being logically tiny, moving the check ahead of default color resolution
regressed the weighted score and most workloads. The accepted placement after
resolving default colors is retained.

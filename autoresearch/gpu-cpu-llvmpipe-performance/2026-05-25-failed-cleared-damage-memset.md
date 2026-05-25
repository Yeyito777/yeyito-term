# Failed experiment: mark cleared-frame damage history with one memset

## Hypothesis

When the GPU path performs a full-frame clear, `xstartdraw()` calls `tfulldirt()`,
so the subsequent draw pass redraws every terminal row. The accepted renderer still
marks the current damage-history row one line at a time inside `gpudrawline()`. For
full-clear frames, marking the whole current damage row with one `memset(..., 1, ...)`
and skipping per-line damage writes might reduce row-renderer overhead while
preserving GLX buffer-age repair correctness.

## Patch summary

In `x.c`, after a full GPU clear, the experiment marked the current damage-history
array as fully dirty:

```c
if (gpu.doublebuf && gpu.damage[0])
    memset(gpu.damage[gpu.damageidx], 1,
           gpu.damagerows * sizeof(*gpu.damage[gpu.damageidx]));
```

In `render/gpu.c`, `gpudrawline()` skipped the per-row damage mark while
`gpu.clearedframe` was set:

```c
if (!gpu.clearedframe && gpu.doublebuf && gpu.damage[0] &&
    BETWEEN(y, 0, gpu.damagerows - 1))
    gpu.damage[gpu.damageidx][y] = 1;
```

Actual GPU rendering, dirty/full-redraw behavior, buffer-age fallback semantics,
fractional scaling, glyph/emoji rendering, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
The benchmark still compared same-source Xft and GPU paths under llvmpipe; it did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/cleared-damage-memset/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.866258`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8456 | 1.6803 | 1.8516 | 1.008x | 1.026x | 1.000x |
| repaint | 0.8331 | 1.2288 | 1.8324 | 0.984x | 1.024x | 1.001x |
| scroll_ascii | 0.9946 | 0.9827 | 1.8649 | 1.009x | 0.997x | 1.000x |
| scroll_unicode | 0.9391 | 1.0732 | 1.8269 | 0.987x | 1.028x | 0.998x |
| scroll_emoji | 1.1402 | 0.7692 | 1.8690 | 1.006x | 0.997x | 0.999x |

## Decision

Rejected and reverted.

The change improved cursor, ASCII, and emoji wall ratios slightly, but repaint and
unicode wall regressed enough to lower the weighted score. Keep the accepted
per-line damage marking behavior.

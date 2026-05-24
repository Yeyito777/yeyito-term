# Failed experiment: cache default GPU colors per frame

## Hypothesis

`gpudrawline()` computes the default foreground/background colors for every row.
Since the default colors are frame-global, caching them once in GPU renderer state
at batch reset time might reduce repeated palette lookup/memcpy work in the hot
line-drawing path, especially after the accepted default-background clear skip.

## Patch summary

The experiment added frame-local cached colors to `Gpu`:

```c
float dfg[3], dbg[3];
```

`gpubatchreset()` filled them once per GPU frame:

```c
gpucolor(defaultfg, gpu.dfg);
gpucolor(defaultbg, gpu.dbg);
```

`gpudrawline()` then used `gpu.dfg` / `gpu.dbg` instead of local per-row default
color arrays, including for cleared-default-background run skipping.

This preserved the actual GPU renderer path, fractional scaling, GPU text/emoji
behavior, triangle batches, alpha test, solid no-blend behavior, and the accepted
cleared-background optimization. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/frame-default-color-cache/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.856750`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8332 | 1.6685 | 1.8498 | 0.992x | 1.007x | 1.000x |
| repaint | 0.8186 | 1.2567 | 1.8312 | 0.985x | 1.021x | 0.999x |
| scroll_ascii | 0.9703 | 0.9876 | 1.8678 | 0.991x | 0.991x | 1.001x |
| scroll_unicode | 0.9446 | 1.0716 | 1.8285 | 1.009x | 1.000x | 1.000x |
| scroll_emoji | 1.1354 | 0.7639 | 1.8698 | 0.992x | 1.002x | 1.000x |

## Decision

Rejected and reverted.

Caching the defaults globally did not pay for itself: weighted score fell and
both cursor/repaint wall ratios regressed. The original row-local default color
resolution is clearer and faster in this benchmark state.

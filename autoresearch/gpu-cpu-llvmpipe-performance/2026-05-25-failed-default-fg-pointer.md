# Failed experiment: use default foreground pointer in GPU line fast path

## Hypothesis

In the accepted default-background fast path, cells with `g.fg == defaultfg` copy
`dfg` into the local `fg` array with `memcpy()` before passing it to glyph and
decoration batching. Most benchmark text uses the default foreground. Tracking a
foreground pointer (`fgp`) that points directly at `dfg` for default-foreground
cells could avoid that per-cell copy while preserving exact colors.

## Patch summary

In `render/gpu.c`, `gpudrawline()` gained a `float *fgp` pointer. For the
accepted default-background fast path:

```c
if (g.fg == defaultfg)
    fgp = dfg;
else {
    gpucolor(g.fg, fg);
    fgp = fg;
}
```

The slow path called `gpuresolve()` and set `fgp = fg`. Text and decoration
batching then used `fgp` instead of `fg`.

This preserved the actual GPU renderer path, fractional scaling, glyph/emoji
rendering, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/default-fg-pointer/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.859480`  
Relative score: `0.988x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8440 | 1.6290 | 1.8476 | 1.006x | 0.994x | 0.998x |
| repaint | 0.8143 | 1.2515 | 1.8291 | 0.962x | 1.043x | 0.999x |
| scroll_ascii | 0.9852 | 0.9943 | 1.8657 | 1.000x | 1.008x | 1.000x |
| scroll_unicode | 0.9359 | 1.0638 | 1.8294 | 0.984x | 1.019x | 1.000x |
| scroll_emoji | 1.1231 | 0.7772 | 1.8665 | 0.990x | 1.007x | 0.997x |

## Decision

Rejected and reverted.

Avoiding the default-foreground copy helped cursor wall slightly, but repaint and
unicode wall regressed badly and the weighted score fell below the accepted
vimnav-guard state. The extra pointer plumbing is not justified.

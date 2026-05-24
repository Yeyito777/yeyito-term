# Failed experiment: skip foreground color for plain spaces

## Hypothesis

In the fast default-background path of `gpudrawline()`, plain space cells without
underline/strikethrough do not draw a glyph or decoration, so their foreground
color is unused. Avoiding default foreground copy / palette lookup for those
cells might reduce per-cell CPU work, especially in repaint and scroll workloads
with padded trailing spaces.

## Patch summary

In `render/gpu.c`, changed the fast default-background branch to compute `fg`
only when it would be consumed by a glyph or decoration:

```c
if (g.u != ' ' || (g.mode & (ATTR_UNDERLINE|ATTR_STRUCK))) {
    if (g.fg == defaultfg)
        memcpy(fg, dfg, sizeof fg);
    else
        gpucolor(g.fg, fg);
}
memcpy(bg, dbg, sizeof bg);
```

This preserved background resolution, glyph drawing, underline/strike drawing,
triangle batches, alpha test, solid no-blend behavior, the accepted clear-color
cache, and the cleared-default-background skip. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/lazy-space-foreground/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.860401`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8468 | 1.6456 | 1.8473 | 1.018x | 0.982x | 0.998x |
| repaint | 0.8041 | 1.2726 | 1.8324 | 0.960x | 1.045x | 1.001x |
| scroll_ascii | 0.9863 | 0.9940 | 1.8628 | 0.994x | 0.999x | 0.999x |
| scroll_unicode | 0.9619 | 1.0452 | 1.8303 | 1.012x | 0.983x | 1.001x |
| scroll_emoji | 1.1423 | 0.7724 | 1.8705 | 0.997x | 0.998x | 1.001x |

## Decision

Rejected and reverted.

Cursor and unicode wall ratios improved, but repaint regressed badly and the
weighted score fell. The extra branch in the hot fast path outweighed the saved
foreground color work for the accepted benchmark state.

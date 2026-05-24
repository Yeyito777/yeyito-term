# Failed experiment: reusable glyph upload scratch buffer

## Hypothesis

Each newly cached GPU glyph allocates a temporary tightly-packed bitmap buffer,
uploads it with `glTexSubImage2D()`, and then frees it. Since every benchmark run
starts fresh processes and therefore rebuilds glyph atlases, reusing one renderer
scratch buffer for glyph uploads might reduce malloc/free overhead during glyph
cache warmup without changing atlas contents or text/emoji behavior.

## Patch summary

The experiment added a scratch buffer to `Gpu`:

```c
unsigned char *scratch;
size_t scratchcap;
```

plus a `gpuscratch(size_t)` helper that grows the buffer with `xrealloc()`.
Both monochrome/gray glyph uploads and color emoji uploads used this scratch
buffer instead of `xmalloc()` / `free()` for every glyph. The buffer was freed in
`gpudestroy()`.

This preserved the actual GPU renderer path, glyph atlas layout, alpha processing,
color emoji cropping/upload behavior, fractional scaling, the accepted clear-color
cache, cleared-background skip, triangle batches, alpha test, and solid no-blend
behavior. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/glyph-upload-scratch/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.864442`  
Relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8496 | 1.6500 | 1.8477 | 1.022x | 0.985x | 0.999x |
| repaint | 0.8256 | 1.2215 | 1.8314 | 0.985x | 1.003x | 1.001x |
| scroll_ascii | 0.9740 | 1.0153 | 1.8645 | 0.982x | 1.021x | 1.000x |
| scroll_unicode | 0.9495 | 1.0510 | 1.8289 | 0.999x | 0.989x | 1.001x |
| scroll_emoji | 1.1365 | 0.7722 | 1.8684 | 0.992x | 0.998x | 1.000x |

## Decision

Rejected and reverted.

The change improved cursor wall, but it slightly lowered total score and regressed
repaint/ASCII wall ratios. The simple per-glyph temporary allocation remains good
enough for the accepted benchmark state.

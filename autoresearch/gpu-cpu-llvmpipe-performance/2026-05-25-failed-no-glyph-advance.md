# Failed experiment: remove unused GPU glyph advance field

## Hypothesis

`GpuGlyph.advance` is assigned from the FreeType glyph advance but is not used by
the GPU renderer; terminal cell layout comes from st's grid and glyph bitmaps are
positioned with `left`/`top`. Removing the unused field and assignment could shrink
glyph metadata and reduce glyph-load work without changing output.

## Patch summary

In `render/gpu.c`, the experiment changed `GpuGlyph` from:

```c
int left, top, advance;
```

to:

```c
int left, top;
```

and removed:

```c
g->advance = face->glyph->advance.x >> 6;
```

Actual glyph lookup/loading, glyph positioning, atlas upload, fractional scaling,
glyph/emoji rendering, batching, accepted clear-color cache, cleared-background
skip, and accepted vimnav row guard were otherwise unchanged. The benchmark still
compared same-source Xft and actual GPU paths under llvmpipe; it did not fallback
to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-glyph-advance/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.855304`  
Relative score: `0.983x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8374 | 1.6336 | 1.8481 | 0.998x | 0.997x | 0.998x |
| repaint | 0.8256 | 1.2344 | 1.8309 | 0.975x | 1.028x | 1.000x |
| scroll_ascii | 0.9564 | 1.0329 | 1.8629 | 0.970x | 1.048x | 0.999x |
| scroll_unicode | 0.9373 | 1.0733 | 1.8292 | 0.985x | 1.028x | 1.000x |
| scroll_emoji | 1.1152 | 0.7899 | 1.8677 | 0.984x | 1.024x | 0.998x |

## Decision

Rejected and reverted.

Although the field is unused, removing it changed code/data layout in a way that
regressed most workloads and reduced the weighted score. Keep the accepted
`GpuGlyph` layout.

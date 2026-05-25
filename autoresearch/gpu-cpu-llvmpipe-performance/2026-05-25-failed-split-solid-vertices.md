# Failed experiment: split solid batches into a smaller vertex format

## Hypothesis

Background, decoration, and cursor-outline batches are solid-color geometry and do
not need texture coordinates. The accepted renderer stores them in the same
`GpuVertex` format as textured glyph batches, so every solid vertex carries unused
`u/v` floats. Splitting solid batches into a smaller `{x, y, r, g, b, a}` vertex
format could reduce client-array bandwidth and llvmpipe vertex processing work for
background-heavy frames while preserving all GPU rendering behavior.

## Patch summary

The experiment added:

```c
typedef struct { GLfloat x, y, r, g, b, a; } GpuSolidVertex;
typedef struct { GpuSolidVertex *v; int len, cap; } GpuSolidBatch;
```

and changed `gpu.bg`, `gpu.deco`, `gpu.obg`, and `gpu.odeco` to use solid batches.
`gpubatchrect()` wrote six solid vertices directly, `gpudrawsolidbatch()` drew
those batches with vertex/color arrays only, and `xfinishdraw()` used the solid
batch draw helper for background/deco/overlay solid batches. Text and color-text
batches kept the accepted textured `GpuVertex` layout and draw path.

This preserved the actual GPU renderer path, triangle batching, glyph/emoji
rendering, fractional scaling, alpha test for textured batches, solid no-blend
behavior, accepted clear-color cache, cleared-background skip, and accepted vimnav
row guard. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/split-solid-vertices/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.857972`  
Relative score: `0.986x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8290 | 1.7123 | 1.8486 | 0.988x | 1.045x | 0.998x |
| repaint | 0.8418 | 1.2062 | 1.8312 | 0.994x | 1.005x | 1.001x |
| scroll_ascii | 0.9763 | 1.0048 | 1.8647 | 0.991x | 1.019x | 1.000x |
| scroll_unicode | 0.9330 | 1.0689 | 1.8285 | 0.980x | 1.024x | 0.999x |
| scroll_emoji | 1.1042 | 0.7840 | 1.8691 | 0.974x | 1.016x | 0.999x |

## Decision

Rejected and reverted.

Although the solid vertex format is smaller, splitting the batch types and draw
helpers did not pay off under llvmpipe. Cursor, ASCII, unicode, and emoji wall
ratios regressed, and CPU ratios generally worsened. The accepted unified vertex
format remains better, likely due to simpler code layout and llvmpipe's handling
of the existing interleaved client arrays.

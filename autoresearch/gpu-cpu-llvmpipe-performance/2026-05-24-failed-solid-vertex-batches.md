# Failed experiment: separate solid-color vertex batches

## Hypothesis

Background and decoration batches do not use texture coordinates, but the unified
`GpuVertex` still stores `u/v` for every solid vertex. Splitting solid-color
batches into a smaller vertex format might reduce client-array bandwidth and
memory/cache pressure under llvmpipe.

## Patch summary

The experiment added a separate solid batch path in `render/gpu.c`:

- `GpuSolidVertex` with only `x, y, r, g, b, a`,
- `GpuSolidBatch` for background/deco/overlay background/overlay deco,
- `gpusolidbatchrect()` to emit triangle rectangles without `u/v`,
- `gpudrawsolidbatch()` using only vertex and color arrays,
- `xfinishdraw()` used `gpudrawsolidbatch()` for solid batches and kept
  `gpudrawbatch()` for textured text/color-emoji batches.

This preserved the actual GPU renderer path, triangle batching, fractional
scaling, glyph atlases, and emoji behavior. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-vertex-batches/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.708495`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5829 | 2.8210 | 1.8513 | 0.990x | 0.989x | 1.000x |
| repaint | 0.7044 | 1.6015 | 1.8312 | 0.998x | 1.043x | 1.001x |
| scroll_ascii | 0.9017 | 1.1277 | 1.8617 | 0.992x | 1.011x | 0.999x |
| scroll_unicode | 0.8447 | 1.2361 | 1.8268 | 0.993x | 0.994x | 1.001x |
| scroll_emoji | 1.0827 | 0.8397 | 1.8699 | 1.011x | 0.990x | 1.000x |

## Decision

Rejected and reverted.

The smaller solid vertex format did not improve weighted score and regressed the
high-priority cursor wall ratio. The extra code paths/state setup likely outweighed
any bandwidth benefit in llvmpipe's client-array path. Keep the simpler unified
vertex format.

# Failed experiment: cache blend and texture enable state

## Hypothesis

After accepting disabled blending for solid batches, the renderer toggles
`GL_BLEND` and `GL_TEXTURE_2D` as it moves between solid and textured batches.
Caching the current enable state in renderer state and skipping redundant
`glEnable()` / `glDisable()` calls might reduce llvmpipe state validation work.

## Patch summary

The experiment added `gpu.blendon` and `gpu.texon` state bits in `render/gpu.c`.
`gpudrawbatch()` only called `glEnable(GL_BLEND)`, `glDisable(GL_BLEND)`,
`glEnable(GL_TEXTURE_2D)`, or `glDisable(GL_TEXTURE_2D)` when the cached state
said the GL state actually needed to change. `xfinishdraw()` updated `gpu.texon`
after the final texture disable.

This preserved the actual GPU renderer path, triangle batches, fractional
scaling, glyph atlases, color emoji blending, and the accepted no-blend solid
batch behavior. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/blend-texture-enable-cache/result.json`

## Result versus accepted disable-solid-blend state

Accepted score: `0.750395`  
Experiment score: `0.746723`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6451 | 2.4544 | 1.8504 | 1.000x | 0.996x | 1.000x |
| repaint | 0.7366 | 1.4279 | 1.8302 | 0.989x | 0.994x | 1.001x |
| scroll_ascii | 0.9014 | 1.1050 | 1.8615 | 0.977x | 1.023x | 0.998x |
| scroll_unicode | 0.8860 | 1.1675 | 1.8276 | 1.002x | 1.001x | 1.001x |
| scroll_emoji | 1.0924 | 0.8272 | 1.8688 | 1.007x | 1.000x | 0.999x |

## Decision

Rejected and reverted.

The branch/cache bookkeeping did not improve the weighted score and hurt repaint
and ASCII scrolling. Explicit GL enable/disable calls remain better for the
accepted llvmpipe renderer state.

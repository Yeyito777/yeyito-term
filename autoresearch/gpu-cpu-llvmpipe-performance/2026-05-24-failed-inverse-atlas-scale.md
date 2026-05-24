# Failed experiment: cached inverse atlas dimensions

## Hypothesis

Every glyph batch computes texture coordinates by dividing atlas pixel positions
by `gpu.atlasw` / `gpu.atlash`. Since accepted atlas dimensions are fixed at
1024x1024 for the context, caching inverse atlas dimensions in renderer state and
multiplying in `gpubatchglyph()` might reduce per-glyph CPU cost.

## Patch summary

In `render/gpu.c`:

- added `gpu.invatlasw` and `gpu.invatlash`,
- initialized them alongside `gpu.atlasw` / `gpu.atlash`,
- changed `gpubatchglyph()` texture coordinate computation from division by atlas
  dimensions to multiplication by the cached inverses,
- fixed the nearby over-indentation of the atlas-size assignments while touching
  that code.

The actual GPU renderer path, accepted alpha test, solid no-blend path, triangle
batches, fractional scaling, glyph atlas rendering, and color emoji behavior were
otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/inverse-atlas-scale/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.746584`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6407 | 2.5069 | 1.8538 | 0.982x | 1.037x | 1.001x |
| repaint | 0.7357 | 1.4664 | 1.8318 | 0.981x | 1.016x | 1.001x |
| scroll_ascii | 0.9297 | 1.0867 | 1.8649 | 1.009x | 1.000x | 1.001x |
| scroll_unicode | 0.8841 | 1.1663 | 1.8270 | 0.975x | 1.040x | 0.999x |
| scroll_emoji | 1.0882 | 0.8226 | 1.8686 | 0.999x | 1.001x | 0.999x |

## Decision

Rejected and reverted.

The multiplication form regressed the weighted score and both high-priority
cursor/repaint workloads. The compiler likely already strength-reduces constant
1024 divisions well enough, and adding state did not help. Keep the simpler
explicit division by atlas dimensions.

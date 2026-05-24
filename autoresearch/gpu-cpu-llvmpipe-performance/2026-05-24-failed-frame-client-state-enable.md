# Failed experiment: enable vertex/color client state once per frame

## Hypothesis

`gpudrawbatch()` enables `GL_VERTEX_ARRAY` and `GL_COLOR_ARRAY` for each batch.
Those states are needed for every GPU batch in a frame. Enabling them once in
`xfinishdraw()` before drawing all batches, then letting each batch only update
its pointers, might reduce llvmpipe GL state-change overhead.

## Patch summary

The experiment moved these calls out of `gpudrawbatch()`:

```c
glEnableClientState(GL_VERTEX_ARRAY);
glEnableClientState(GL_COLOR_ARRAY);
```

and into the GPU branch of `xfinishdraw()` immediately before the batch draw
sequence. Texture-coordinate state handling stayed per textured/untextured batch.

This preserved the actual GPU renderer path, triangle batches, fractional
scaling, and glyph/emoji behavior. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/frame-client-state-enable/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.705310`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5856 | 2.8018 | 1.8500 | 0.995x | 0.982x | 0.999x |
| repaint | 0.6986 | 1.5781 | 1.8288 | 0.989x | 1.028x | 0.999x |
| scroll_ascii | 0.8996 | 1.1175 | 1.8659 | 0.989x | 1.002x | 1.001x |
| scroll_unicode | 0.8570 | 1.2182 | 1.8265 | 1.007x | 0.980x | 1.000x |
| scroll_emoji | 1.0347 | 0.8824 | 1.8699 | 0.966x | 1.041x | 1.000x |

## Decision

Rejected and reverted.

The attempted state-change reduction regressed weighted score and high-priority
cursor/repaint wall ratios. The existing per-batch enables appear cheap enough or
better aligned with llvmpipe's state validation path.

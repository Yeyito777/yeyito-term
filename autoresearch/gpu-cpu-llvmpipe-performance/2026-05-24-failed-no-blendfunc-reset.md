# Failed experiment: omit post-texture blend-function reset

## Hypothesis

`gpudrawbatch()` sets the blend function before textured batches, then resets it
to the normal alpha-blend function afterward. Since every textured batch sets the
blend function it needs before drawing, and untextured batches use fully opaque
vertices, omitting the post-batch reset might reduce llvmpipe GL state-change
cost without changing renderer semantics.

## Patch summary

In `render/gpu.c`, removed this post-draw block from `gpudrawbatch()`:

```c
if (textured)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
```

The renderer still used the actual GPU path, triangle batches, fractional
scaling, glyph atlases, and color emoji blending. This was not an Xft fallback or
`gpudraw` bypass.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-blendfunc-reset/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.709605`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5861 | 2.8430 | 1.8498 | 0.996x | 0.997x | 0.999x |
| repaint | 0.7073 | 1.5261 | 1.8328 | 1.002x | 0.994x | 1.002x |
| scroll_ascii | 0.8876 | 1.1499 | 1.8640 | 0.976x | 1.031x | 1.000x |
| scroll_unicode | 0.8618 | 1.2178 | 1.8278 | 1.013x | 0.980x | 1.001x |
| scroll_emoji | 1.0766 | 0.8783 | 1.8686 | 1.005x | 1.036x | 1.000x |

## Decision

Rejected and reverted.

The change did not improve the weighted score and slightly hurt cursor wall while
also regressing ASCII scrolling. The blend reset is not a useful llvmpipe CPU
optimization in the accepted renderer state.

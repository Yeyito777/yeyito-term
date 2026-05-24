# Failed experiment: remove post-textured blend reset after alpha test

## Hypothesis

The accepted renderer sets the blend function at the start of every textured
batch and then resets it to `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` after every
textured draw. Since normal text batches explicitly set the normal blend function
and color emoji batches explicitly set their special `GL_ONE` source factor,
the post-draw reset may be redundant state churn under llvmpipe, especially after
solid batches already disable blending.

## Patch summary

In `render/gpu.c`, removed the trailing reset from `gpudrawbatch()`:

```c
if (textured)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
```

All textured batches still set their intended blend function before drawing, and
solid batches still disable blending and alpha testing. This preserved the actual
GPU renderer path, the accepted textured alpha test, triangle batches, fractional
scaling, glyph atlas rendering, and color emoji behavior. It did not fallback to
Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-blend-reset-after-alpha/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.751403`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6577 | 2.4202 | 1.8513 | 1.008x | 1.001x | 1.000x |
| repaint | 0.7478 | 1.4646 | 1.8290 | 0.997x | 1.015x | 0.999x |
| scroll_ascii | 0.8988 | 1.1106 | 1.8654 | 0.975x | 1.022x | 1.001x |
| scroll_unicode | 0.8786 | 1.1689 | 1.8270 | 0.969x | 1.042x | 0.999x |
| scroll_emoji | 1.1064 | 0.8249 | 1.8711 | 1.016x | 1.003x | 1.000x |

## Decision

Rejected and reverted.

Although cursor wall improved slightly, the weighted score regressed and ASCII /
Unicode scrolling suffered. Repaint CPU also regressed. Keep the accepted explicit
post-textured reset, which is clearer and more stable under the benchmark.

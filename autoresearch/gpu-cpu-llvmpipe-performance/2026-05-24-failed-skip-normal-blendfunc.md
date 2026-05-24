# Failed experiment: skip redundant normal-text blend function calls

## Hypothesis

Normal alpha-text batches (`textured == 1`) always use
`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, which is the renderer's default blend
function and is restored after color emoji batches. Avoiding redundant
`glBlendFunc()` calls for normal text while keeping the special color-emoji
`GL_ONE` blend path might reduce llvmpipe state validation overhead.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` was changed so that:

- normal textured batches bind the main atlas but do not call `glBlendFunc()`,
- color textured batches still switch to `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`,
- only color textured batches restore `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`.

The renderer stayed on the actual GPU path with triangle batches, fractional
scaling, glyph atlases, and emoji rendering intact. This was not an Xft fallback
or `gpudraw` bypass.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-normal-blendfunc/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.711175`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5864 | 2.8243 | 1.8504 | 0.996x | 0.990x | 0.999x |
| repaint | 0.7065 | 1.5479 | 1.8296 | 1.001x | 1.008x | 1.000x |
| scroll_ascii | 0.9000 | 1.1191 | 1.8627 | 0.990x | 1.003x | 0.999x |
| scroll_unicode | 0.8557 | 1.2226 | 1.8290 | 1.005x | 0.983x | 1.002x |
| scroll_emoji | 1.0674 | 0.8458 | 1.8696 | 0.997x | 0.998x | 1.000x |

## Decision

Rejected and reverted.

The result was close but still below the accepted weighted score, and cursor wall
ratio regressed slightly. Since the acceptance rule requires improving the
weighted score without hurting high-priority workloads, this state-change
reduction does not qualify.

# Failed experiment: set alpha function once during GPU init

## Hypothesis

The accepted alpha-test path sets `glAlphaFunc(GL_GREATER, 0.0f)` for every
textured batch. The alpha comparison function is stable renderer state, so moving
that call to `gpuinit()` and leaving `gpudrawbatch()` to only enable/disable
`GL_ALPHA_TEST` might reduce llvmpipe state-validation overhead.

## Patch summary

In `render/gpu.c`:

- removed `glAlphaFunc(GL_GREATER, 0.0f)` from the textured branch of
  `gpudrawbatch()`,
- added the same `glAlphaFunc()` call during `gpuinit()` after the default blend
  setup.

The actual GPU renderer path, textured alpha testing, solid no-blend path,
triangle batches, fractional scaling, glyph atlas rendering, and color emoji path
were otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/alpha-func-init/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.756478`  
Relative score: `1.000x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6522 | 2.4501 | 1.8492 | 0.999x | 1.014x | 0.999x |
| repaint | 0.7346 | 1.4717 | 1.8273 | 0.980x | 1.020x | 0.998x |
| scroll_ascii | 0.9171 | 1.0973 | 1.8623 | 0.995x | 1.010x | 0.999x |
| scroll_unicode | 0.8656 | 1.1841 | 1.8279 | 0.955x | 1.056x | 1.000x |
| scroll_emoji | 1.1882 | 0.7515 | 1.8689 | 1.091x | 0.914x | 0.999x |

## Decision

Rejected and reverted.

The weighted score was barely positive only because the emoji workload improved
substantially in this run. The two highest-weight workloads did not improve:
cursor was essentially flat with worse CPU, and repaint regressed about 2% in
wall time and CPU. That violates the acceptance rule for no material
cursor/repaint regression, so the clearer accepted per-textured-batch alpha setup
remains.

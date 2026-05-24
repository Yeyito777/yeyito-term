# Failed experiment: skip normal-text blend function after disabling solid blending

## Hypothesis

After accepting disabled blending for solid batches, normal text batches still
call `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`. Since this is the
renderer's default and color-emoji batches restore it, skipping that call for
normal text might reduce llvmpipe state validation overhead while preserving the
special color-emoji blend path.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` was changed so that:

- textured normal glyph batches enable blending and bind the main atlas but do
  not call `glBlendFunc()`,
- color emoji batches still switch to `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`,
- only color emoji batches restore `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`,
- solid batches still draw with blending disabled from the accepted state.

This preserved the actual GPU renderer path, triangle batches, fractional
scaling, glyph atlases, and emoji behavior. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-noblend-skip-normal-blendfunc/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-noblend-skip-normal-blendfunc-validate/result.json`

## Validation result versus accepted disable-solid-blend state

Accepted score: `0.750395`  
Validation score: `0.745408`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6402 | 2.5024 | 1.8508 | 0.992x | 1.016x | 1.000x |
| repaint | 0.7306 | 1.4902 | 1.8283 | 0.981x | 1.038x | 1.000x |
| scroll_ascii | 0.9342 | 1.0516 | 1.8637 | 1.012x | 0.973x | 0.999x |
| scroll_unicode | 0.8786 | 1.1953 | 1.8256 | 0.993x | 1.024x | 1.000x |
| scroll_emoji | 1.0869 | 0.8198 | 1.8697 | 1.002x | 0.991x | 0.999x |

## Decision

Rejected and reverted.

The initial run looked promising, but the validation run did not reproduce. It
regressed the weighted score and hurt the high-priority cursor and repaint wall
ratios. Keep the accepted, simpler blend setup: textured batches explicitly set
their blend function, and solid batches disable blending.

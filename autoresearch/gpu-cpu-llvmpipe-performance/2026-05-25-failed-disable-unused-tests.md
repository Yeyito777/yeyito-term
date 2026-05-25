# Failed experiment: disable unused fixed-function tests in GPU init

## Hypothesis

The renderer draws 2D triangle batches without culling, depth, stencil, or scissor
semantics. Explicitly disabling `GL_CULL_FACE`, `GL_DEPTH_TEST`, `GL_STENCIL_TEST`,
and `GL_SCISSOR_TEST` during GPU initialization could avoid any unexpected llvmpipe
fixed-function test overhead while preserving output.

## Patch summary

In `render/gpu.c`, the experiment added these GL state setup calls in `gpuinit()`
after pixel unpack setup:

```c
glDisable(GL_CULL_FACE);
glDisable(GL_DEPTH_TEST);
glDisable(GL_STENCIL_TEST);
glDisable(GL_SCISSOR_TEST);
```

No batching, glyph/emoji rendering, fractional scaling, accepted clear-color cache,
cleared-background skip, or accepted vimnav row guard was changed. The benchmark
still compared same-source Xft and actual GPU paths under llvmpipe; it did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-unused-tests/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868305`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8293 | 1.6663 | 1.8489 | 0.988x | 1.017x | 0.998x |
| repaint | 0.8423 | 1.1831 | 1.8301 | 0.995x | 0.986x | 1.000x |
| scroll_ascii | 0.9927 | 1.0001 | 1.8643 | 1.007x | 1.014x | 0.999x |
| scroll_unicode | 0.9389 | 1.0734 | 1.8290 | 0.987x | 1.028x | 1.000x |
| scroll_emoji | 1.1322 | 0.7090 | 1.8692 | 0.998x | 0.919x | 0.999x |

## Decision

Rejected and reverted.

The explicit disables were close overall and helped ASCII wall, but cursor,
unicode, and emoji wall ratios were below accepted and the weighted score still
lost. Keep the accepted GL state setup.

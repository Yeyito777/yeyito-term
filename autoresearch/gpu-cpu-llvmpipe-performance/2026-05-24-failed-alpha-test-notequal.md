# Failed experiment: use GL_NOTEQUAL for textured alpha test

## Hypothesis

The accepted textured-batch alpha test uses `glAlphaFunc(GL_GREATER, 0.0f)` to
reject fully transparent texels. For normalized texture alpha, `GL_NOTEQUAL, 0.0f`
should be equivalent for the renderer's atlas data and might map to a cheaper
llvmpipe comparison.

## Patch summary

In `render/gpu.c`, changed the textured branch of `gpudrawbatch()` from:

```c
glAlphaFunc(GL_GREATER, 0.0f);
```

to:

```c
glAlphaFunc(GL_NOTEQUAL, 0.0f);
```

The actual GPU renderer path, alpha testing, solid no-blend path, triangle
batches, fractional scaling, glyph atlas rendering, and color emoji path were
otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/alpha-test-notequal/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/alpha-test-notequal-validate/result.json`

## Validation result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Initial score: `0.764172`  
Validation score: `0.740176`  
Validation relative score: `0.979x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6517 | 2.4509 | 1.8506 | 0.999x | 1.014x | 0.999x |
| repaint | 0.7235 | 1.4977 | 1.8297 | 0.965x | 1.038x | 1.000x |
| scroll_ascii | 0.8802 | 1.1345 | 1.8650 | 0.955x | 1.044x | 1.001x |
| scroll_unicode | 0.8831 | 1.1694 | 1.8286 | 0.974x | 1.043x | 1.000x |
| scroll_emoji | 1.0986 | 0.8294 | 1.8695 | 1.009x | 1.009x | 0.999x |

## Decision

Rejected and reverted.

The initial run looked promising, but validation did not reproduce and materially
regressed the weighted score plus repaint and ASCII scrolling. Keep the accepted
`GL_GREATER, 0.0f` alpha test.

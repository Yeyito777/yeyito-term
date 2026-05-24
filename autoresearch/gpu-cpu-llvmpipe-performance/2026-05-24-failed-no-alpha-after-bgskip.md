# Failed experiment: disable alpha test after cleared-background skip

## Hypothesis

After accepting the much larger optimization that skips redundant default
backgrounds after a full-frame clear, the earlier accepted textured-batch alpha
test might no longer be helpful. Removing alpha testing for textured batches could
avoid fixed-function alpha-test state/fragment overhead while relying on normal
texture blending for glyph and emoji transparency.

## Patch summary

In `render/gpu.c`, changed the textured branch of `gpudrawbatch()` from enabling
alpha testing:

```c
glEnable(GL_ALPHA_TEST);
glAlphaFunc(GL_GREATER, 0.0f);
```

to explicitly disabling it:

```c
glDisable(GL_ALPHA_TEST);
```

The actual GPU renderer path, accepted cleared-background skip, solid no-blend
behavior, triangle batches, fractional scaling, glyph atlas rendering, and color
emoji path were otherwise unchanged. This did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-alpha-after-bgskip/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-alpha-after-bgskip-validate/result.json`

## Validation result versus accepted skipped-cleared-background state

Accepted score: `0.863260`  
Initial score: `0.864044`  
Validation score: `0.857392`  
Validation relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8372 | 1.6516 | 1.8488 | 0.997x | 0.997x | 0.999x |
| repaint | 0.8137 | 1.2741 | 1.8306 | 0.979x | 1.035x | 0.999x |
| scroll_ascii | 0.9737 | 1.0019 | 1.8654 | 0.995x | 1.005x | 0.999x |
| scroll_unicode | 0.9510 | 1.0665 | 1.8290 | 1.016x | 0.995x | 1.001x |
| scroll_emoji | 1.1409 | 0.7590 | 1.8688 | 0.997x | 0.995x | 0.999x |

## Decision

Rejected and reverted.

The initial run was slightly positive, but validation did not reproduce and
materially regressed repaint wall time and CPU. The accepted alpha-test path still
matters after the cleared-background optimization, so keep it enabled for
textured batches.

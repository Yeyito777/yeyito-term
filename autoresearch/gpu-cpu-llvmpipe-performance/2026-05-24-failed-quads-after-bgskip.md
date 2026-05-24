# Failed experiment: restore GL_QUADS after cleared-background skip

## Hypothesis

The earlier accepted triangle-batch change replaced `GL_QUADS` with client-side
expansion to `GL_TRIANGLES`. After the later accepted cleared-background skip,
the batch mix changed substantially and fewer solid background quads are drawn.
With fewer quads overall, reducing vertex count from six triangle vertices back
to four quad vertices might outweigh llvmpipe's legacy `GL_QUADS` handling cost.

## Patch summary

In `render/gpu.c`, changed `gpubatchquad()` back to emitting four vertices per
quad and changed `gpudrawbatch()` from:

```c
glDrawArrays(GL_TRIANGLES, 0, b->len);
```

to:

```c
glDrawArrays(GL_QUADS, 0, b->len);
```

The actual GPU renderer path, accepted cleared-background skip, alpha test, solid
no-blend behavior, fractional scaling, glyph atlas rendering, and color emoji
path were otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/quads-after-bgskip/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/quads-after-bgskip-validate/result.json`

## Validation result versus accepted cleared-background state

Accepted score: `0.863260`  
Initial score: `0.875641`  
Validation score: `0.861627`  
Validation relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8472 | 1.6349 | 1.8528 | 1.009x | 0.987x | 1.001x |
| repaint | 0.8170 | 1.2482 | 1.8319 | 0.983x | 1.014x | 1.000x |
| scroll_ascii | 0.9755 | 1.0060 | 1.8606 | 0.997x | 1.009x | 0.997x |
| scroll_unicode | 0.9471 | 1.0608 | 1.8272 | 1.012x | 0.990x | 1.000x |
| scroll_emoji | 1.1385 | 0.7677 | 1.8691 | 0.995x | 1.007x | 1.000x |

## Decision

Rejected and reverted.

The initial run looked promising, but validation did not reproduce and repaint
regressed materially. The accepted triangle-batch path remains the better and
more modern primitive path under the current benchmark state.

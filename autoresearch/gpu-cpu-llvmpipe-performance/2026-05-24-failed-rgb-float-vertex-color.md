# Failed experiment: RGB-only float vertex colors

## Hypothesis

Every GPU vertex stores RGBA color as four floats, but the renderer always uses
alpha `1.0f`. Removing the alpha field and using a three-component
`glColorPointer(3, GL_FLOAT, ...)` might reduce client-array bandwidth and vertex
batch memory under llvmpipe without changing visible output.

## Patch summary

In `render/gpu.c`, the experiment:

- changed `GpuVertex` from `r, g, b, a` to `r, g, b`,
- removed the constant `1.0f` alpha from batch vertex initializers,
- changed color array setup from:

```c
glColorPointer(4, GL_FLOAT, sizeof(GpuVertex), coff);
```

to:

```c
glColorPointer(3, GL_FLOAT, sizeof(GpuVertex), coff);
```

The GPU renderer path, triangle batching, fractional scaling, glyph atlases, and
emoji behavior stayed intact. This was not an Xft fallback or `gpudraw` bypass.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/rgb-float-vertex-color/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/rgb-float-vertex-color-validate/result.json`

## Validation result versus accepted triangle-batch state

Accepted score: `0.712339`  
Validation score: `0.705349`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5844 | 2.8036 | 1.8517 | 0.993x | 0.983x | 1.000x |
| repaint | 0.7062 | 1.5506 | 1.8289 | 1.000x | 1.010x | 1.000x |
| scroll_ascii | 0.8796 | 1.1589 | 1.8623 | 0.968x | 1.039x | 0.999x |
| scroll_unicode | 0.8620 | 1.2077 | 1.8270 | 1.013x | 0.972x | 1.001x |
| scroll_emoji | 1.0410 | 0.8753 | 1.8697 | 0.972x | 1.032x | 1.000x |

## Decision

Rejected and reverted.

The initial 7-iteration run looked positive, but the 9-iteration validation run
did not reproduce and regressed the weighted score. It also hurt cursor wall and
ASCII/emoji wall ratios. The alpha component may be cheap for llvmpipe's existing
array path, or the smaller stride may interact less favorably with its vectorized
fetch/format conversion.

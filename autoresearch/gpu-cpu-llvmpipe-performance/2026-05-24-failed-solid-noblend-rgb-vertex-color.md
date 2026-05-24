# Failed experiment: RGB-only vertex colors after disabling solid blending

## Hypothesis

All GPU vertices carry an alpha component fixed at `1.0f`. After accepting the
solid-batch no-blend optimization, removing that unused alpha component and using
`glColorPointer(3, GL_FLOAT, ...)` might reduce client-array bandwidth and cache
footprint under llvmpipe.

## Patch summary

In `render/gpu.c`, the experiment:

- changed `GpuVertex` from `r, g, b, a` to `r, g, b`,
- removed the constant `1.0f` alpha from batch vertex initializers,
- changed color-array setup from:

```c
glColorPointer(4, GL_FLOAT, sizeof(GpuVertex), coff);
```

to:

```c
glColorPointer(3, GL_FLOAT, sizeof(GpuVertex), coff);
```

This preserved the actual GPU renderer path, triangle batches, fractional
scaling, glyph atlases, color emoji, and the accepted no-blend solid batch
behavior. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-noblend-rgb-vertex-color/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-noblend-rgb-vertex-color-validate/result.json`

## Validation result versus accepted disable-solid-blend state

Accepted score: `0.750395`  
Validation score: `0.746099`  
Relative score: `0.994x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6496 | 2.5065 | 1.8503 | 1.007x | 1.017x | 1.000x |
| repaint | 0.7384 | 1.4602 | 1.8274 | 0.992x | 1.017x | 0.999x |
| scroll_ascii | 0.9142 | 1.0845 | 1.8625 | 0.990x | 1.004x | 0.999x |
| scroll_unicode | 0.8757 | 1.1772 | 1.8254 | 0.990x | 1.009x | 1.000x |
| scroll_emoji | 1.0781 | 0.8356 | 1.8690 | 0.994x | 1.011x | 0.999x |

## Decision

Rejected and reverted.

The initial run looked positive, but the validation run did not reproduce. It
regressed the weighted score and hurt repaint plus scrolling wall ratios. The
4-float color path remains better for llvmpipe in the accepted renderer state.

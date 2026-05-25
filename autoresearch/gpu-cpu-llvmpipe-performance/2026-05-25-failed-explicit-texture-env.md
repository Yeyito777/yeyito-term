# Failed experiment: explicitly set fixed-function texture environment

## Hypothesis

The fixed-function GPU renderer relies on the default texture environment mode,
which is `GL_MODULATE`, to combine glyph texture alpha/color with vertex color.
Explicitly setting `GL_TEXTURE_ENV_MODE` to `GL_MODULATE` once during GPU
initialization might avoid driver-side default-state uncertainty or lazy state
setup in llvmpipe.

## Patch summary

In `render/gpu.c`, the experiment added during `gpuinit()` after texture parameter
setup:

```c
glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
```

No drawing paths, batching, glyph/emoji rendering, fractional scaling, blend/alpha
state, accepted clear-color cache, cleared-background skip, or accepted vimnav row
guard were changed. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/explicit-texture-env/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.862344`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8332 | 1.6238 | 1.8524 | 0.993x | 0.991x | 1.000x |
| repaint | 0.8427 | 1.2208 | 1.8338 | 0.995x | 1.017x | 1.002x |
| scroll_ascii | 0.9654 | 1.0328 | 1.8671 | 0.980x | 1.047x | 1.001x |
| scroll_unicode | 0.9250 | 1.0570 | 1.8283 | 0.972x | 1.012x | 0.999x |
| scroll_emoji | 1.1171 | 0.7224 | 1.8702 | 0.985x | 0.936x | 0.999x |

## Decision

Rejected and reverted.

The explicit texture environment state was behavior-preserving but did not help
llvmpipe. It regressed the weighted score and several wall-time workloads, so the
accepted implicit/default texture environment remains better.

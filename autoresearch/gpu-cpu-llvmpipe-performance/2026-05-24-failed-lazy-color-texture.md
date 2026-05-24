# Failed experiment: lazily create color emoji texture object

## Hypothesis

The accepted renderer already lazily allocates color emoji atlas storage with
`glTexImage2D()` only when the first color glyph is uploaded, but `gpuinit()` still
creates and configures the color texture object on every GPU startup. Delaying
`glGenTextures()` and color-texture parameter setup until the first color glyph
might reduce startup/frame overhead for ASCII/unicode workloads that never draw
color emoji.

## Patch summary

In `render/gpu.c`, the experiment removed color texture creation/parameter setup
from `gpuinit()`. In `gpuglyph()`, the color-glyph branch created/configured the
texture on first use:

```c
if (!gpu.catlas) {
    glGenTextures(1, &gpu.catlas);
    glBindTexture(GL_TEXTURE_2D, gpu.catlas);
    glTexParameteri(... GL_LINEAR ...);
    glTexParameteri(... GL_CLAMP_TO_EDGE ...);
} else {
    glBindTexture(GL_TEXTURE_2D, gpu.catlas);
}
```

The monochrome/gray glyph branch explicitly bound `gpu.atlas` before upload. The
existing lazy color atlas storage allocation (`gpu.catlasready`) was preserved.

This stayed on the actual GPU renderer path and preserved glyph/emoji behavior,
fractional scaling, triangle batches, alpha test, solid no-blend behavior, the
accepted clear-color cache, and cleared-background skip. It did not fallback to
Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/lazy-color-texture/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/lazy-color-texture-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.863660`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8415 | 1.6310 | 1.8509 | 1.012x | 0.974x | 1.000x |
| repaint | 0.8297 | 1.2298 | 1.8311 | 0.990x | 1.010x | 1.001x |
| scroll_ascii | 0.9869 | 0.9906 | 1.8656 | 0.995x | 0.996x | 1.000x |
| scroll_unicode | 0.9396 | 1.0597 | 1.8286 | 0.988x | 0.997x | 1.000x |
| scroll_emoji | 1.1220 | 0.7768 | 1.8686 | 0.979x | 1.004x | 1.000x |

## Decision

Rejected and reverted.

The initial run looked strongly positive (`0.897113`), but it did not reproduce:
validation fell below the accepted clear-color state cache. Cursor wall improved,
but repaint, scrolling, and emoji wall ratios regressed. Keeping texture object
creation in `gpuinit()` remains better for the accepted benchmark state.

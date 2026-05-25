# Failed experiment: remove frame-end GL cleanup

## Hypothesis

At the end of every GPU frame, `xfinishdraw()` disables vertex, color, and texture
coordinate client arrays plus `GL_TEXTURE_2D`. The next frame's batch drawing code
already sets up the state it needs. Removing this frame-end cleanup might reduce
fixed-function GL state traffic under llvmpipe without changing rendered output.

## Patch summary

In `x.c`, the experiment removed these calls from the GPU branch of
`xfinishdraw()`:

```c
glDisableClientState(GL_VERTEX_ARRAY);
glDisableClientState(GL_COLOR_ARRAY);
glDisableClientState(GL_TEXTURE_COORD_ARRAY);
glDisable(GL_TEXTURE_2D);
```

The per-batch state setup in `gpudrawbatch()` remained unchanged, including solid
batch texture/alpha/blend disables and textured batch texture coordinate setup.
The actual GPU renderer path, triangle batches, glyph/emoji behavior, fractional
scaling, alpha test, solid no-blend behavior, accepted clear-color cache, and
cleared-background skip were preserved. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-frame-gl-cleanup/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.861557`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8388 | 1.6333 | 1.8500 | 1.009x | 0.975x | 1.000x |
| repaint | 0.8339 | 1.2197 | 1.8299 | 0.995x | 1.001x | 1.000x |
| scroll_ascii | 0.9600 | 1.0165 | 1.8643 | 0.968x | 1.022x | 1.000x |
| scroll_unicode | 0.9403 | 1.0680 | 1.8282 | 0.989x | 1.005x | 1.000x |
| scroll_emoji | 1.1320 | 0.7663 | 1.8689 | 0.988x | 0.991x | 1.000x |

## Decision

Rejected and reverted.

The cleanup removal helped cursor wall slightly, but it regressed total score,
repaint wall, and especially ASCII scrolling. The explicit frame-end cleanup is
not the current bottleneck and remains preferable for this benchmark state.

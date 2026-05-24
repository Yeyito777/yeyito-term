# Failed experiment: cache texture coordinate client state

## Hypothesis

`gpudrawbatch()` enables the texture coordinate client array for every textured
batch and disables it for every solid batch. Since consecutive textured batches
(normal text then color emoji, and overlay text then overlay emoji) use the same
client array state, tracking whether `GL_TEXTURE_COORD_ARRAY` is enabled might
avoid redundant client-state changes under llvmpipe.

## Patch summary

In `render/gpu.c` / `x.c`:

- added `gpu.texcoordon`,
- enabled `GL_TEXTURE_COORD_ARRAY` only when transitioning from off to on,
- disabled it only when transitioning from on to off,
- reset `gpu.texcoordon` after the unconditional end-of-frame client-state
  disable in `xfinishdraw()`.

This preserved the actual GPU renderer path, accepted alpha testing, solid
no-blend behavior, triangle batches, fractional scaling, glyph atlas rendering,
and color emoji path. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/texcoord-client-state-cache/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.749584`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6601 | 2.4371 | 1.8503 | 1.011x | 1.008x | 0.999x |
| repaint | 0.7280 | 1.4865 | 1.8293 | 0.971x | 1.030x | 0.999x |
| scroll_ascii | 0.9222 | 1.0781 | 1.8624 | 1.001x | 0.993x | 0.999x |
| scroll_unicode | 0.8870 | 1.1621 | 1.8264 | 0.978x | 1.036x | 0.999x |
| scroll_emoji | 1.0866 | 0.8261 | 1.8687 | 0.998x | 1.005x | 0.999x |

## Decision

Rejected and reverted.

Cursor improved slightly, but repaint regressed materially and the weighted score
fell. The extra branch/state tracking is not worth the saved client-state calls;
keep the simpler explicit enable/disable path.

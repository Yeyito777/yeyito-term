# Failed experiment: cache bound GPU texture

## Hypothesis

The GPU renderer binds the same glyph textures repeatedly while uploading glyphs
and drawing batches. Caching the currently-bound texture in renderer state and
skipping redundant `glBindTexture()` calls might reduce llvmpipe state-change CPU
cost without changing rendering behavior.

## Patch summary

The experiment added a `gpu.boundtex` field and a `gpubindtexture()` helper:

```c
if (gpu.boundtex != tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    gpu.boundtex = tex;
}
```

It replaced renderer-local `glBindTexture(GL_TEXTURE_2D, ...)` calls in atlas
setup/reset, glyph upload, and batch drawing. The GPU renderer path, batching,
fractional scaling, and emoji behavior were otherwise unchanged; this was not an
Xft fallback or `gpudraw` bypass.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/texture-bind-cache/result.json`

## Result versus accepted 2048-slack state

Accepted score: `0.712101`  
Experiment score: `0.702903`  
Relative score: `0.987x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5855 | 2.8138 | 1.8510 | 1.003x | 1.009x | 1.000x |
| repaint | 0.6866 | 1.6035 | 1.8302 | 0.971x | 1.023x | 1.000x |
| scroll_ascii | 0.9016 | 1.1219 | 1.8620 | 0.982x | 1.014x | 1.001x |
| scroll_unicode | 0.8526 | 1.2221 | 1.8266 | 0.995x | 0.997x | 0.999x |
| scroll_emoji | 1.0526 | 0.8803 | 1.8674 | 0.988x | 1.021x | 0.999x |

## Decision

Rejected and reverted.

The change slightly helped cursor, but materially regressed repaint and the
overall weighted score. The extra branch/state tracking likely outweighed any
llvmpipe benefit from avoiding redundant binds. Keeping direct `glBindTexture()`
calls is better for the accepted benchmark state.

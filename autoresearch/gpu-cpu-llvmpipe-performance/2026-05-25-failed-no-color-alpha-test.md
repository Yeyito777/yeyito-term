# Failed experiment: disable alpha test for color-text batches

## Hypothesis

The accepted textured batch path enables `GL_ALPHA_TEST` for both grayscale text
and BGRA color emoji. For premultiplied BGRA color emoji blended with
`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`, transparent pixels should contribute nothing, so
alpha testing may be redundant for `textured == 2`. Disabling alpha test only for
color-text batches could reduce llvmpipe raster work/state cost while preserving
appearance.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` was changed from enabling alpha test for all
textured batches to:

```c
if (textured == 2) {
    glDisable(GL_ALPHA_TEST);
} else {
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.0f);
}
```

Other textured state, color emoji blending, grayscale text alpha testing, triangle
batches, actual GPU path, fractional scaling, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were unchanged. It did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-color-alpha-test/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.863469`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8337 | 1.6323 | 1.8526 | 0.993x | 0.996x | 1.000x |
| repaint | 0.8175 | 1.2386 | 1.8313 | 0.966x | 1.032x | 1.001x |
| scroll_ascii | 1.0002 | 0.9692 | 1.8653 | 1.015x | 0.983x | 1.000x |
| scroll_unicode | 0.9392 | 1.0670 | 1.8282 | 0.987x | 1.022x | 0.999x |
| scroll_emoji | 1.1538 | 0.7653 | 1.8694 | 1.018x | 0.992x | 0.999x |

## Decision

Rejected and reverted.

Disabling alpha test for color-text batches helped emoji and ASCII wall ratios,
but cursor, repaint, and unicode regressed enough to lower the weighted score. The
accepted path keeps alpha testing enabled for all textured batches.

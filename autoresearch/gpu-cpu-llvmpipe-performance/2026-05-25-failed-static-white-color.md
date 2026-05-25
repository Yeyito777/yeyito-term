# Failed experiment: static white color for color glyph batches

## Hypothesis

Color emoji glyph batches always use white vertex color because the BGRA glyph
texture carries its own color. `gpudrawline()` and `gpudrawcell()` each created a
local `{1.0f, 1.0f, 1.0f}` array for this. Hoisting that constant to a shared
static array and making batch color inputs `const` could remove small per-call
stack initialization work in the hot renderer.

## Patch summary

In `render/gpu.c`, the experiment added:

```c
static const float gpuwhite[3] = {1.0f, 1.0f, 1.0f};
```

and changed `gpubatchquad()`, `gpubatchrect()`, and `gpubatchglyph()` to accept
`const float c[3]`. The color-emoji calls in `gpudrawline()` and `gpudrawcell()`
used `gpuwhite` instead of local `white` arrays.

This preserved the actual GPU renderer path, color emoji output, fractional
scaling, glyph rendering, triangle batches, alpha test, solid no-blend behavior,
accepted clear-color cache, and cleared-background skip. It did not fallback to
Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/static-white-color/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.858137`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8367 | 1.6610 | 1.8496 | 1.006x | 0.992x | 1.000x |
| repaint | 0.8118 | 1.2601 | 1.8292 | 0.969x | 1.034x | 1.000x |
| scroll_ascii | 0.9751 | 0.9910 | 1.8674 | 0.983x | 0.996x | 1.001x |
| scroll_unicode | 0.9373 | 1.0690 | 1.8289 | 0.986x | 1.006x | 1.001x |
| scroll_emoji | 1.1241 | 0.7050 | 1.8689 | 0.981x | 0.911x | 1.000x |

## Decision

Rejected and reverted.

The constant hoist was harmless functionally but did not improve performance.
Repaint and emoji regressed materially, and the weighted score fell below the
accepted clear-color cache state. Keep the existing local white arrays.

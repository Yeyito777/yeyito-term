# Failed experiment: remove unused x parameter from gpuresolve

## Hypothesis

`gpuresolve()` only uses the row coordinate for vimnav/debug background handling;
the column argument is unused. Removing the unused `x` parameter could slightly
simplify call sites and generated code in the hot row/cell rendering paths.

## Patch summary

In `render/gpu.c`, the experiment changed:

```c
static void gpuresolve(Glyph g, int x, int y, float fg[3], float bg[3])
```

to:

```c
static void gpuresolve(Glyph g, int y, float fg[3], float bg[3])
```

and updated the `gpudrawline()` / `gpudrawcell()` call sites accordingly.

Actual color resolution semantics, vimnav/debug handling, selected/search marks,
fractional scaling, glyph/emoji rendering, batching, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were otherwise unchanged.
The benchmark still compared same-source Xft and GPU paths under llvmpipe; it did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/gpuresolve-no-x/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.855821`  
Relative score: `0.983x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8278 | 1.7005 | 1.8512 | 0.986x | 1.038x | 1.000x |
| repaint | 0.8263 | 1.2351 | 1.8285 | 0.976x | 1.029x | 0.999x |
| scroll_ascii | 0.9907 | 0.9897 | 1.8649 | 1.005x | 1.004x | 1.000x |
| scroll_unicode | 0.9196 | 1.0800 | 1.8264 | 0.966x | 1.034x | 0.998x |
| scroll_emoji | 1.1229 | 0.7765 | 1.8715 | 0.990x | 1.006x | 1.000x |

## Decision

Rejected and reverted.

The cleanup did not improve generated-code behavior under the benchmark. It
regressed cursor, repaint, unicode, and emoji wall ratios enough to lower the
weighted score, so the accepted signature remains unchanged.

# Failed experiment: retry hoisted default-background row predicate after clear-color cache

## Hypothesis

A previous experiment hoisted row-wide default-background predicates from the hot
per-cell condition in `gpudrawline()` and was rejected against the cleared-background
frontier because repaint regressed despite a slight total-score gain. After the
accepted clear-color state cache changed frame setup costs, the tradeoff might have
shifted enough to make the hoist worthwhile.

## Patch summary

In `render/gpu.c`, `gpudrawline()` computed once per row:

```c
int fastdefault = !IS_SET(MODE_REVERSE) && !debug_mode && y != vimline;
```

and replaced the per-cell row-wide checks in the default-background fast path with
`fastdefault`.

The actual GPU renderer path, fractional scaling, glyph/emoji behavior, triangle
batches, alpha test, solid no-blend behavior, accepted clear-color cache, and
cleared-background skip were otherwise unchanged. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/fastdefault-clearcolor/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.856362`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8305 | 1.6443 | 1.8491 | 0.999x | 0.982x | 0.999x |
| repaint | 0.8268 | 1.2295 | 1.8310 | 0.987x | 1.009x | 1.001x |
| scroll_ascii | 0.9789 | 1.0372 | 1.8633 | 0.987x | 1.043x | 0.999x |
| scroll_unicode | 0.9262 | 1.0775 | 1.8293 | 0.974x | 1.014x | 1.001x |
| scroll_emoji | 1.1296 | 0.7833 | 1.8687 | 0.986x | 1.012x | 1.000x |

## Decision

Rejected and reverted.

After the clear-color cache, the row predicate hoist performed worse than before:
the weighted score and high-priority repaint workload both regressed. Keep the
accepted inline condition in `gpudrawline()`.

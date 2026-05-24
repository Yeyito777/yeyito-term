# Failed experiment: correctness-preserving cleared empty-row skip

## Hypothesis

The earlier cleared-empty-row fast path used `tlinelen(y) == 0` to skip rows on a
fully-cleared frame. That is fast, but `tlinelen()` only checks trailing spaces;
it does not prove that all cells are visually inert. A row containing spaces with
custom backgrounds or underline/strike attributes could still be visible. A
correct fast path should scan the row and skip only if every cell is an ordinary
space with default background and no attributes.

## Patch summary

In `render/gpu.c`, replaced the `tlinelen(y) == 0` empty-row condition with a
full-span scan in `gpudrawline()`:

```c
emptyclear = 1;
for (x = x1; x < x2; x++) {
    if (line[x].u != ' ' || line[x].bg != defaultbg || line[x].mode != ATTR_NULL) {
        emptyclear = 0;
        break;
    }
}
if (emptyclear)
    return;
```

The guard still only applied on fully-cleared frames with selection/search/reverse
/debug/vimnav row effects inactive. The actual GPU renderer path, cleared default
background skip, alpha test, solid no-blend behavior, triangle batches,
fractional scaling, glyph atlas rendering, and color emoji path were otherwise
unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/correct-empty-row-skip/result.json`

## Result versus the previous `tlinelen()` empty-row state

Previous score: `0.875049`  
Experiment score: `0.859259`  
Relative score: `0.982x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs previous | CPU vs previous | RSS vs previous |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8369 | 1.6410 | 1.8484 | 0.992x | 0.987x | 1.000x |
| repaint | 0.8181 | 1.2424 | 1.8324 | 0.983x | 1.014x | 0.999x |
| scroll_ascii | 0.9778 | 0.9997 | 1.8643 | 0.995x | 1.016x | 1.000x |
| scroll_unicode | 0.9416 | 1.0712 | 1.8282 | 1.000x | 1.006x | 1.000x |
| scroll_emoji | 1.1382 | 0.7683 | 1.8707 | 0.920x | 1.083x | 1.001x |

## Decision

Rejected as a performance optimization.

The correctness-preserving scan is slower than both the unsafe `tlinelen()` check
and the prior accepted cleared-background-only state. Because correctness matters,
the unsafe `tlinelen()` empty-row optimization is retracted separately rather than
kept.

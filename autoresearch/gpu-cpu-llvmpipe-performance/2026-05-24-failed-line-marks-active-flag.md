# Failed experiment: row marks-active flag

## Hypothesis

Most benchmark frames have neither an active selection nor active search matches.
`gpudrawline()` still evaluates two per-cell conditional branches guarded by
`selactive` and `searchactive`. Hoisting their combined state into a single
`marksactive` branch might reduce per-cell branch work in the common no-mark row
case.

## Patch summary

In `render/gpu.c`, `gpudrawline()` added:

```c
int marksactive = searchactive || selactive;
```

and wrapped the per-cell selection/search updates in:

```c
if (marksactive) {
    if (selactive && selected(x, y))
        g.mode |= ATTR_SELECTED;
    if (searchactive && search_matched(x, y))
        g.mode |= ATTR_MATCH;
}
```

This preserved selection/search behavior, the actual GPU renderer path, the
accepted cleared-background skip, triangle batches, alpha test, solid no-blend,
and text/emoji rendering. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/line-marks-active-flag/result.json`

## Result versus accepted cleared-background state

Accepted score: `0.863260`  
Experiment score: `0.854990`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8312 | 1.6469 | 1.8466 | 0.990x | 0.994x | 0.998x |
| repaint | 0.8173 | 1.2187 | 1.8298 | 0.984x | 0.990x | 0.999x |
| scroll_ascii | 0.9626 | 1.0245 | 1.8653 | 0.984x | 1.028x | 0.999x |
| scroll_unicode | 0.9464 | 1.0659 | 1.8274 | 1.011x | 0.995x | 1.000x |
| scroll_emoji | 1.1249 | 0.7858 | 1.8685 | 0.983x | 1.030x | 0.999x |

## Decision

Rejected and reverted.

The extra combined branch made the hot no-mark path slower overall and regressed
both high-priority cursor and repaint wall ratios. The original two simple guarded
branches remain better for this benchmark state.

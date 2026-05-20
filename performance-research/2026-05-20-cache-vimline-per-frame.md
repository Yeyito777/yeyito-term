# Experiment: cache vim navigation highlight row per GPU frame

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

`gpudrawline()` calls `vimnav_curline_y()` once per dirty row.  The value is
frame-stable, so caching it in the GPU frame/batch state might reduce per-row
work for repaint and scroll workloads.

## Patch tested

In `render/gpu.c`:

- added `gpu.vimline`,
- set it once in `gpubatchreset()` with `vimnav_curline_y()`,
- used `gpu.vimline` in `gpudrawline()` instead of calling
  `vimnav_curline_y()` per row.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.988x` | `1.016x` | regression |
| repaint | `1.012x` | `1.009x` | wall tiny positive, CPU slight regression |
| scroll_ascii | `1.005x` | `0.978x` | tiny positive |

## Decision

Rejected and reverted.

The change produced small wins for repaint/scroll, but it regressed the cursor
workload, which is the main blocker.  The overall effect is too small/noisy to
keep.

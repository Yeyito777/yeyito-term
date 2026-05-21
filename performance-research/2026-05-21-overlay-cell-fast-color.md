# Experiment: fast color path for overlay cell draws

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

`gpudrawcell()` is used by cursor overlay restore/draw.  It always calls the full
`gpuresolve()` path, while `gpudrawline()` has a common default-background fast
path.  Applying a similar fast path to overlay cell drawing could reduce cursor
CPU cost without changing visual behavior.

## Patch tested

In `render/gpu.c` inside `gpudrawcell()`:

- for default-background cells without selection/match/reverse/faint/blink/etc.,
  resolve foreground/background directly through `gpucolor()`;
- otherwise keep the existing `gpuresolve()` behavior.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output: `/tmp/st-ab-cellfastpath-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0047x` | `0.9814x` | small A/B cursor win |
| repaint | `0.9927x` | `0.6938x` | CPU win, wall regression |
| scroll_ascii | `1.0227x` | `0.9962x` | small wall win |

## Fair GPU-vs-Xft benchmark

Benchmark output: `/tmp/st-quick-bench-fair-cellfastpath.jsonl`.

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft | Result |
|---|---:|---:|---|
| scroll_ascii | `1.0242x` | `0.8043x` | good |
| scroll_unicode | `0.9787x` | `1.0154x` | regression |
| scroll_emoji | `1.2063x` | `0.7993x` | good |
| repaint | `0.9206x` | `0.7143x` | CPU good, wall worse than baseline |
| cursor_updates | `0.8051x` | `1.1940x` | worse than baseline cursor CPU |

## Decision

Rejected and reverted.

The direct GPU A/B looked mildly promising, but the fair benchmark regressed the
remaining blockers: cursor CPU became worse than the Option C baseline and repaint
wall-time moved further away from Xft.  Do not retry this overlay fast-color path
in the same form.

Post-revert validation:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.  The working tree was clean and the installed binary still matched
the clean repo build.

# Experiment: skip XIM spot updates in GPU draw path

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

The cursor-update workload changes the terminal cursor frequently.  At the end of
`draw()`, cursor movement can trigger `xximspot()`, which updates input-method
spot location via `XSetICValues()`.  Since the GPU benchmark does not use active
preedit UI and the input context was created with `XIMPreeditNothing`, skipping
spot updates for the GPU path might reduce cursor-update overhead.

## Patch tested

In `x.c`, added an early return to `xximspot()` when the GPU renderer is active:

```c
if (gpu.active)
    return;
```

This was intentionally tested as a behavior-risky optimization because some input
methods may still use the spot location for candidate placement even when st uses
`XIMPreeditNothing`.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

After rejection and revert:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-skipximspot-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.9920x` | `1.0058x` | cursor wall/CPU regression |
| repaint | `0.9957x` | `0.9856x` | small CPU win, wall regression |
| scroll_ascii | `1.0096x` | `1.0164x` | wall win, CPU regression |

## Decision

Rejected and reverted.

The benchmark did not support the hypothesis: cursor got worse, repaint wall got
worse, and scroll CPU regressed.  It also carried input-method behavior risk, so
it should not be kept or retried in this form.

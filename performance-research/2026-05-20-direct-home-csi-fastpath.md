# Experiment: direct cursor move for bare `ESC[H`

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

The repaint workload emits bare `ESC[H` once per frame.  The existing fast CSI path
for `ESC[H` still calls `tmoveato(0, 0)`.  Replacing that call with direct cursor
state updates could reduce repaint parser overhead while preserving origin-mode
behavior.

## Patch tested

In `st.c`, inside `tfastcsi()` for bare `H` with no arguments, replaced:

```c
tmoveato(0, 0);
```

with direct cursor state updates:

- clear `CURSOR_WRAPNEXT`,
- set `term.c.x = 0`,
- set `term.c.y = term.top` when `CURSOR_ORIGIN` is active, otherwise `0`.

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
| cursor_updates | `0.988x` | `1.030x` | regression |
| repaint | `1.019x` | `1.426x` | wall win, CPU regression |
| scroll_ascii | `1.015x` | `0.968x` | small win |

## Decision

Rejected and reverted.

The patch improved repaint/scroll wall in this run, but it regressed cursor wall and
CPU, and repaint CPU regressed badly.  Since cursor remains the primary blocker and
repaint CPU is already unstable, this is not a no-regression improvement.

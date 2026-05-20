# Experiment: direct cursor positioning inside cursor-color fast path

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

The GPU cursor-color fast path still calls `tmoveato(col - 1, row - 1)` for every
recognized cursor update.  That helper handles origin mode and bounds clamping.
Inlining the equivalent cursor movement in `tfastcursorop()` could reduce parser
hot-path overhead for the cursor benchmark.

## Patch tested

In `st.c`, inside `tfastcursorop()`, replaced:

```c
tmoveato(col - 1, row - 1);
```

with direct cursor state updates:

- clear `CURSOR_WRAPNEXT`,
- clamp `term.c.x`,
- clamp `term.c.y`, respecting `CURSOR_ORIGIN`.

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
| cursor_updates | `0.990x` | `0.679x` | CPU win, wall regression |
| repaint | `0.999x` | `0.992x` | near tie |
| scroll_ascii | `0.988x` | `1.008x` | regression |

## Decision

Rejected and reverted.

The direct move reduced cursor CPU but regressed cursor wall time and ASCII scroll.
Cursor wall time is the primary blocker, so this is not acceptable.

# Experiment: guard `selected()` in `tputc()`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

A longer repaint profile showed `selected()` in the parser/write path, even though
normal benchmark runs do not have an active selection.  The fast ASCII path already
checks `sel.ob.x != -1` before calling `selected()`.  Adding the same guard to the
full `tputc()` path might reduce Unicode repaint overhead with no behavior change.

## Patch tested

In `st.c`, changed:

```c
if (selected(term.c.x, term.c.y))
    selclear();
```

to:

```c
if (sel.ob.x != -1 && selected(term.c.x, term.c.y))
    selclear();
```

This preserves behavior: when no selection exists, `selected()` would return 0;
when a selection exists, the old check still runs.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test
make -C ~/Config/st test_gpu_regressions
```

Result: passed with the patch applied.

After rejection and revert:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-selectguard-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.9999x` | `1.0104x` | effectively neutral wall, CPU regression |
| repaint | `1.0043x` | `1.0239x` | tiny wall win, CPU regression |
| scroll_ascii | `0.9982x` | `1.0098x` | wall/CPU regression |

## Decision

Rejected and reverted.

The code simplification was behavior-preserving and tests passed, but the measured
CPU impact was negative and wall-time impact was too small/noisy.  Since this does
not improve the target benchmark set, it is not worth keeping.

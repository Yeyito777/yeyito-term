# Experiment: precheck printable ASCII before `tputcfastascii`

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

The GPU parser path calls `tputcfastascii()` for every byte before falling back to
UTF-8/control handling.  Adding a cheap printable-ASCII guard in `twrite()` could
avoid entering the fast ASCII helper for escape/control/non-ASCII bytes and reduce
parser overhead in cursor/repaint workloads.

## Patch tested

In `st.c`, changed the GPU fast-ASCII call site from an unconditional helper call
to a guarded call:

```c
if (gpu && !show_ctrl && BETWEEN((uchar)buf[n], 0x20, 0x7e) &&
    tputcfastascii((uchar)buf[n])) {
    ...
}
```

No renderer behavior was intended to change; this only filtered which bytes can
enter the existing fast ASCII helper.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-asciicheck-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0119x` | `1.4868x` | tiny wall win, severe CPU regression |
| repaint | `1.0199x` | `0.9943x` | small wall win, CPU near tie |
| scroll_ascii | `0.9946x` | `0.9762x` | wall regression/noise |

## Decision

Rejected and reverted.

The wall-time improvements for cursor and repaint were too small to justify the
large cursor CPU regression and the scroll wall regression/noise.  Since cursor
CPU is already one of the fair-benchmark blockers, this change should not be kept
or retried in the same form.

Post-revert validation:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.  The working tree was clean and the installed binary still matched
the clean repo build.

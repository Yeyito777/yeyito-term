# Experiment: force-inline `tputcfastascii()`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

Recent Option C profiles continued to show parser/write-side cost in the repaint
workload, with `tputcfastascii()` and its callers visible in the hot path.  Since
this function is small and called very frequently, forcing it inline might reduce
call overhead and improve repaint/cursor throughput without changing renderer
behavior.

## Patch tested

In `st.c`:

- changed the `tputcfastascii()` prototype to `static inline` with
  `__attribute__((always_inline))`,
- changed the function definition to `static inline`,
- made no semantic changes to the write path.

## Validation

With the patch applied:

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

Benchmark output was saved to `/tmp/st-ab-inlinefastascii-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.9999x` | `1.0046x` | neutral/slightly worse |
| repaint | `1.0034x` | `1.5350x` | tiny wall win, severe CPU regression |
| scroll_ascii | `1.0170x` | `0.9848x` | small win |

## Decision

Rejected and reverted.

The small scroll and repaint wall improvements did not justify the large repaint
CPU regression, and cursor updates did not improve.  This suggests the compiler's
existing inlining choices are already good enough, or that forcing this function
inline increases code size/cache pressure in the hot write path.

Do not retry forced inlining of `tputcfastascii()` in this form.

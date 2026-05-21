# Optimization: branch hint for GPU ASCII fast path

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

Profiles for repaint continue to show `tputcfastascii()` as part of the hot
write path.  In GPU mode, the caller only reaches this helper on the optimized
printable path, so the early rejection case is usually cold for ASCII-heavy
workloads.  Marking that rejection as unlikely may improve code layout/branch
prediction without changing behavior.

## Patch kept

In `st.c`:

- added local `likely()` / `unlikely()` wrappers using `__builtin_expect`,
- wrapped the early rejection condition in `tputcfastascii()` with `unlikely()`.

No terminal semantics or renderer behavior are intended to change.

## Validation

With the patch applied:

```sh
make -C ~/Config/st
make -C ~/Config/st test
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-unlikelyfastascii-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0139x` | `0.9963x` | small cursor win |
| repaint | `0.9960x` | `1.0008x` | tiny repaint regression/noise |
| scroll_ascii | `1.0189x` | `1.0025x` | small scroll wall win |

## Fair GPU-vs-Xft check

Benchmark output was saved to `/tmp/st-quick-bench-fair-unlikelyfastascii.jsonl`.

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft | Result |
|---|---:|---:|---|
| scroll_ascii | `1.0668x` | `0.8899x` | win |
| scroll_unicode | `0.9864x` | `0.7955x` | wall near-tie, CPU win |
| scroll_emoji | `1.2238x` | `0.6802x` | strong win |
| repaint | `0.9232x` | `0.6886x` | wall still behind, CPU win |
| cursor_updates | `0.8403x` | `0.8118x` | wall still behind, CPU win |

## Decision

Kept.

The change is small, semantics-preserving, and improves direct GPU A/B cursor
and ASCII-scroll wall time with only a tiny repaint wall regression that is within
run-to-run noise.  The fair benchmark still shows cursor and repaint wall-time as
blockers, but cursor CPU is favorable in this run and no serious broad regression
was observed.

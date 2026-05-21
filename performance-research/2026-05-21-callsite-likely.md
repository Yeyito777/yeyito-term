# Failed experiment: likely() around GPU fast ASCII call site

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `0dfae25 Hint GPU ASCII fast path branch`

## Hypothesis

After the accepted `tputcfastascii()` branch-hint patch, the next hot branch in
`twrite()` was the GPU fast ASCII call site itself:

```c
if (gpu && !show_ctrl && tputcfastascii((uchar)buf[n]))
```

The experiment wrapped this whole condition in `likely(...)`, expecting the GPU
renderer's common printable path to benefit from branch layout.

## Validation while patched

The patched tree built and passed targeted GPU regression tests:

```sh
make
make test_gpu_regressions
```

## A/B against the current GPU baseline

Benchmark output: `/tmp/st-ab-calllikely-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 0.9977 | 0.6725 | CPU looked better, wall slightly worse |
| repaint | 1.0105 | 0.7026 | promising in direct A/B |
| scroll_ascii | 0.9976 | 0.9904 | roughly neutral/slightly worse |

## Fair GPU-vs-Xft benchmark while patched

Benchmark output: `/tmp/st-quick-bench-fair-calllikely.jsonl`

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft |
|---|---:|---:|
| scroll_ascii | 1.0628 | 0.9024 |
| scroll_unicode | 1.0172 | 0.9894 |
| scroll_emoji | 1.2247 | 0.6848 |
| repaint | 0.9208 | 0.7228 |
| cursor_updates | 0.8314 | 1.3481 |

## Decision

Rejected.

The direct A/B result was tempting, especially repaint CPU, but the fair
GPU-vs-Xft run made cursor CPU substantially worse than the accepted branch-hint
baseline.  Cursor updates are still one of the primary blockers, so accepting a
patch with this fair-benchmark profile would be too risky.

The code change was reverted; this note is kept so the experiment is not
retried in the same form.

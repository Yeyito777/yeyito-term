# Experiment: simple-line branch in GPU row draw

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

Most benchmark rows are not selected, not in search mode, not in debug mode, not
reverse-video, and not the vim-navigation current line.  `gpudrawline()` still
checks several of those conditions per cell.  Computing a row-level `simpleline`
flag could reduce per-cell branch work and improve cursor/repaint/scroll.

## Patch tested

In `render/gpu.c`:

- computed `simpleline = !selactive && !searchactive && !debug_mode && !MODE_REVERSE && y != vimline`,
- skipped per-cell selection/search checks when `simpleline` is true,
- used the default-background fast path only under `simpleline`.

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
| cursor_updates | `0.992x` | `0.676x` | CPU positive, wall slightly worse |
| repaint | `0.999x` | `0.989x` | near tie / tiny CPU positive |
| scroll_ascii | `1.006x` | `1.010x` | wall tiny positive, CPU tiny regression |

## Fair GPU-vs-Xft spot check with patch

The patch did not change the broader blocker picture:

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft |
|---|---:|---:|
| scroll_ascii | `1.022x` | `0.973x` |
| scroll_unicode | `0.989x` | `1.184x` |
| scroll_emoji | `1.171x` | `0.657x` |
| repaint | `0.912x` | `0.730x` |
| cursor_updates | `0.798x` | `1.500x` |

## Decision

Rejected and reverted.

The CPU improvements were workload/noise dependent, but cursor wall time regressed
slightly in A/B, Unicode fair CPU regressed, and the cursor/repaint blockers were
not improved.  Do not retry this exact row-level branch factoring without stronger
profiling evidence.

# Current post-cleanup fair benchmark baseline

Date: 2026-05-20
Branch: `gpu-renderer-prototype`
Tip at measurement: `a56509d Log failed eager palette cache experiment`

## Purpose

Capture a fresh GPU-vs-Xft fair benchmark after the renderer move and recent
cleanup/logging commits.  This is a baseline/checkpoint, not a code experiment.

## Method

Built current source twice:

- GPU binary with `gpudraw = 1` as `/tmp/st-bench-gpu`,
- Xft binary with `gpudraw = 0` as `/tmp/st-bench-xft`.

Ran the temporary fair benchmark helper in a fresh `xenv` instance:

```sh
DISPLAY=$disp /tmp/st-quick-bench-fair.py
```

## Results

Median kept-iteration results:

| Workload | Xft wall | GPU wall | GPU wall speedup | GPU CPU ratio | GPU RSS | Result |
|---|---:|---:|---:|---:|---:|---|
| scroll_ascii | `0.5020s` | `0.4747s` | `1.058x` | `1.105x` | `325 MB` | wall win, CPU worse |
| scroll_unicode | `0.4907s` | `0.4939s` | `0.994x` | `0.976x` | `328 MB` | near tie, CPU slight win |
| scroll_emoji | `0.4169s` | `0.3424s` | `1.218x` | `0.679x` | `327 MB` | strong win |
| repaint | `0.2994s` | `0.3217s` | `0.931x` | `1.032x` | `328 MB` | regression/blocker |
| cursor_updates | `0.2425s` | `0.2868s` | `0.846x` | `1.167x` | `326 MB` | regression/blocker |

## Interpretation

The broad picture is unchanged:

- GPU remains good on emoji scroll and generally competitive on scroll.
- GPU startup wins from earlier commits are preserved but not measured in this run.
- Repaint and cursor updates remain the main blockers.
- GPU RSS remains much higher than Xft due to GL/atlas/driver state.

## Decision

Keep as a benchmark checkpoint.  No source-code changes were made for this entry.

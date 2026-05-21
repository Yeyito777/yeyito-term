# Option C stable baseline

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Tip at measurement: `7c89a3a Keep cmdline cursor below overlay border` (`stable` tag)

## Purpose

Capture a fresh fair GPU-vs-Xft benchmark before starting larger renderer/refactor
experiments.  The goal for the next phase is larger wins without visible behavior
changes, runaway CPU/GPU usage, or idle spin.

## Method

Built current source twice:

- GPU binary with `gpudraw = 1` as `/tmp/st-bench-gpu`,
- Xft binary with `gpudraw = 0` as `/tmp/st-bench-xft`.

Ran `/tmp/st-quick-bench-fair.py` in a fresh `xenv` instance.

## Results

Median kept-iteration results:

| Workload | Xft wall | GPU wall | GPU wall speedup | GPU CPU ratio | GPU RSS | Result |
|---|---:|---:|---:|---:|---:|---|
| scroll_ascii | `0.5024s` | `0.4879s` | `1.030x` | `0.998x` | `326 MB` | small wall win, CPU tie |
| scroll_unicode | `0.4902s` | `0.5091s` | `0.963x` | `0.979x` | `328 MB` | wall regression, CPU slight win |
| scroll_emoji | `0.4096s` | `0.3560s` | `1.150x` | `0.724x` | `327 MB` | strong win |
| repaint | `0.3080s` | `0.3177s` | `0.969x` | `0.767x` | `328 MB` | wall slight loss, CPU win |
| cursor_updates | `0.2466s` | `0.2986s` | `0.826x` | `1.085x` | `326 MB` | blocker |

## Interpretation

The stable branch is still tasteful at idle and passes tests, but the same shape
remains:

- emoji and ASCII scrolling are wins,
- repaint CPU can be good but wall-time is still slightly behind,
- cursor updates remain the clearest wall-time blocker,
- GPU RSS remains high but stable due to GL/atlas/driver state.

This is the baseline for larger Option C experiments.

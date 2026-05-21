# Failed experiment: pass active selection/search state into GPU cursor cell draws

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `d84d2c9 Log failed background run key experiment`

## Hypothesis

`gpudrawcursor()` already computes `selection_active()` and `search_active()` once, but its
calls into `gpudrawcell()` recomputed those flags for the old and new cursor cells. Passing
the active flags into an internal `gpudrawcellactive()` helper might reduce cursor overlay CPU
without changing visual behavior.

## Validation while patched

- `make`
- `make test_gpu_regressions`

Both passed.

## A/B result

Output: `/tmp/st-ab-cellactive-two.jsonl`

| Workload | Wall speedup | CPU ratio |
| --- | ---: | ---: |
| cursor_updates | 0.9958x | 0.9896x |
| repaint | 0.9986x | 1.0248x |
| scroll_ascii | 1.0158x | 0.9946x |

## Decision

Rejected.  The small CPU reduction on cursor updates was paired with a cursor wall regression,
and repaint regressed both wall and CPU.  The scroll_ascii gain is not relevant enough to keep
this cursor/repaint-sensitive change.

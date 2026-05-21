# Failed experiment: keyed default-background runs in GPU renderer

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `f79707b Log failed hot write path attribute experiment`

## Hypothesis

`gpudrawline()` builds background rectangles by comparing the current cell
background color against the current run color with `gpucoloreq()`.  Most normal
terminal cells use the default background.  Tracking a simple integer key for
that common default-background path could avoid repeated float color comparisons
when extending background runs.

## Patch tested

In `render/gpu.c`, `gpudrawline()` tracked:

- `bgkey = defaultbg` for the common default-background fast path,
- `bgkey = -1` for fully resolved/complex backgrounds,
- `runkey` for the active background run.

Run extension used the key before falling back to `gpucoloreq()`.

## Validation while patched

```sh
make
make test_gpu_regressions
```

Both passed.

## A/B benchmark

Output: `/tmp/st-ab-bgkey-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 0.9898 | 0.9957 | wall regression |
| repaint | 1.0034 | 0.9846 | small win |
| scroll_ascii | 1.0090 | 0.9665 | small win |

## Fair GPU-vs-Xft snapshot

Output: `/tmp/st-quick-bench-fair-bgkey.jsonl`

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft |
|---|---:|---:|
| scroll_ascii | 1.0586 | 0.8870 |
| scroll_unicode | 1.0111 | 0.9666 |
| scroll_emoji | 1.2423 | 0.7811 |
| repaint | 0.9316 | 1.0268 |
| cursor_updates | 0.8414 | 1.3086 |

## Decision

Rejected.

The direct A/B result helped repaint and scroll slightly, but cursor wall time
regressed and the fair cursor CPU result was poor.  Cursor updates remain a
primary blocker, so the tradeoff is not acceptable.  The code change was
reverted and this note is kept for future reference.

# Experiment: integer coordinate fast path for unscaled GPU windows

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

When the GPU window content area exactly matches the terminal grid size
(`win.w - 2 * borderpx == win.tw` and `win.h - 2 * borderpx == win.th`), the GPU
coordinate helpers do not need floating-point scaling and rounding.  Returning
`borderpx + x * win.cw` / `borderpx + y * win.ch` directly might reduce draw-time
geometry overhead in scroll/repaint/cursor workloads without changing visual
behavior in unscaled windows.

## Patch tested

In `render/gpu.c`, added unscaled fast paths to:

- `gpucellx()`
- `gpucelly()`
- `gpucellright()`
- `gpurowbottom()`

The existing scaled/rounded path remained the fallback for scaled GPU windows.

## Validation

```sh
make -C ~/Config/st
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

Benchmark output was saved to `/tmp/st-ab-integercoords-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0012x` | `1.4879x` | tiny wall win, severe CPU regression |
| repaint | `0.9979x` | `0.9977x` | near tie / slight wall regression |
| scroll_ascii | `1.0162x` | `0.9776x` | useful scroll win |

## Decision

Rejected and reverted.

The scroll result was promising, but the cursor CPU regression was far too large
and cursor remains one of the primary blockers.  The branch is not keeping
optimizations that improve scroll at the expense of cursor/update CPU.

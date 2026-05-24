# Experiment: disable blending for GPU background batches

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

The background batch draws opaque rectangles with per-vertex RGB/RGBA alpha 1.0.
Disabling `GL_BLEND` around untextured background batches could reduce GPU/driver
work for repaint and scroll workloads.

## Patch tested

In `render/gpu.c`, inside `gpudrawbatch()`:

- when `textured == 0`, call `glDisable(GL_BLEND)` before drawing,
- re-enable `GL_BLEND` immediately after drawing the untextured batch.

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
| cursor_updates | `1.006x` | `1.025x` | wall tiny positive, CPU worse |
| repaint | `0.991x` | `1.028x` | regression |
| scroll_ascii | `1.002x` | `1.273x` | CPU regression |

## Decision

Rejected and reverted.

The extra blend state toggles cost more CPU than they save.  Cursor wall improved
slightly, but repaint regressed and scroll CPU regressed badly.

# Experiment: use a static white color for color glyph batches

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

`gpudrawline()` and `gpudrawcell()` each initialize a local white color array for
color emoji glyph drawing.  Replacing those local arrays with a single static
renderer-wide white color could reduce per-row/per-cell stack initialization and
help emoji/repaint paths without changing rendering.

## Patch tested

In `render/gpu.c`:

- added `static float gpuwhite[3] = { 1.0f, 1.0f, 1.0f };`,
- removed local `white[3]` arrays from `gpudrawline()` and `gpudrawcell()`,
- passed `gpuwhite` to `gpubatchglyph()` for color glyph batches.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Two A/B runs were performed because the first was mixed.

First run medians:

| Workload | Wall speedup | CPU ratio |
|---|---:|---:|
| cursor_updates | `1.004x` | `1.022x` |
| repaint | `1.029x` | `0.974x` |
| scroll_ascii | `1.007x` | `1.014x` |

Second run medians:

| Workload | Wall speedup | CPU ratio |
|---|---:|---:|
| cursor_updates | `0.994x` | `0.990x` |
| repaint | `0.998x` | `1.187x` |
| scroll_ascii | `0.983x` | `1.004x` |

## Decision

Rejected and reverted.

The result was unstable/noisy.  A repaint win in the first run did not reproduce;
the second run regressed repaint CPU and scroll wall.  Do not keep.

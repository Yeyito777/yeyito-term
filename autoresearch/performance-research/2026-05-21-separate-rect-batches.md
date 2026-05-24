# Experiment: separate GPU rectangle batches

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

Most background and decoration geometry does not need texture coordinates.  The
current GPU batch vertex carries position, texture coordinates, and RGBA color for
all batches, including opaque background rectangles.  Splitting rectangle batches
from glyph batches could reduce vertex bandwidth and GL client-array setup for
background/decor passes.

## Patch tested

In `render/gpu.c` and `x.c`:

- added a `GpuRectVertex` with only position and color,
- added a `GpuRectBatch` for background/decor/overlay-rect batches,
- changed `gpubatchrect()` to fill rect batches,
- added `gpudrawrectbatch()` without texture coordinate arrays,
- kept glyph/text batches on the existing textured vertex format.

No visual behavior was intended to change.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output: `/tmp/st-ab-rectbatch-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0046x` | `1.4458x` | tiny wall win, severe CPU regression |
| repaint | `1.0205x` | `1.4734x` | wall win, severe CPU regression |
| scroll_ascii | `1.0214x` | `0.8210x` | wall and CPU win |

## Decision

Rejected and reverted.

The split batch representation improved wall time in this A/B and helped ASCII
scroll CPU, but cursor and repaint CPU regressed by roughly 45–47%.  The user
explicitly asked for tasteful improvements that do not drive CPU/GPU usage high,
so this is not acceptable despite the wall-clock wins.

Post-revert validation:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.  The working tree was clean and the installed binary still matched
the clean repo build.

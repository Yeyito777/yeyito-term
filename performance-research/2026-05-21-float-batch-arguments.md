# Experiment: use float batch coordinates and texture coordinates

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

`render/gpu.c` builds many `GpuVertex` entries per frame.  The batch helpers
accepted `double` coordinates and texture coordinates, then stored them as
`GLfloat` fields.  Changing the helper signatures and local texture-coordinate
math to `float` could reduce conversion work in the GPU batch-building path,
especially for repaint and scroll workloads.

## Patch tested

In `render/gpu.c`:

- changed `gpubatchquad()` parameters from `double` to `float`,
- changed `gpubatchrect()` parameters from `double` to `float`,
- changed `gpubatchglyph()` parameters from `double` to `float`,
- changed local glyph texture-coordinate variables from `double` to `float`,
- made no intended visual or renderer-behavior changes.

## Validation

With the patch applied:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

After rejection and revert:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-floatbatch-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.9871x` | `1.0048x` | cursor regression |
| repaint | `1.0019x` | `1.0055x` | tiny wall win, CPU regression |
| scroll_ascii | `1.0088x` | `0.7712x` | scroll win |

## Decision

Rejected and reverted.

Although ASCII scroll improved, cursor updates regressed and repaint CPU worsened.
Cursor remains the primary blocker, so this tradeoff is not acceptable.

Do not retry this broad float-argument conversion in the same form.  A more
localized coordinate optimization would need to avoid the cursor regression.

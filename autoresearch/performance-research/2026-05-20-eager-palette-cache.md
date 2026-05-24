# Experiment: build GPU palette cache once per frame

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

`gpucolor()` checks `gpupalvalid` and can lazily rebuild the Xft-to-float palette
on every call.  Building the palette cache once at the start of each GPU frame
(`gpubatchreset()`) and removing the lazy rebuild branch from `gpucolor()` could
reduce per-cell color resolution overhead.

## Patch tested

In `render/gpu.c`:

- factored palette-cache building into `gpuensurepal()`,
- called `gpuensurepal()` from `gpubatchreset()`,
- simplified `gpucolor()` to assume the palette cache is valid for indexed colors.

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
| cursor_updates | `0.988x` | `1.429x` | regression |
| repaint | `1.014x` | `0.995x` | small win |
| scroll_ascii | `0.995x` | `0.978x` | mixed |

## Decision

Rejected and reverted.

Although repaint improved slightly, the cursor workload regressed in both wall time
and CPU.  Cursor remains the main blocker, so this is not worth keeping.

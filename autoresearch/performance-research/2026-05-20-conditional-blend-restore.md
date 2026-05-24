# Experiment: restore GL blend function only after color glyph batches

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

`gpudrawbatch()` changes the blend function to `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`
only for color glyph batches (`textured == 2`), but restored
`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` after any textured batch.  Restoring only
after color glyph batches should remove one redundant `glBlendFunc()` from normal
text batches and may help repaint/cursor/scroll CPU.

## Patch tested

In `render/gpu.c`:

```c
if (textured)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
```

changed to:

```c
if (textured == 2)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
```

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
| cursor_updates | `1.001x` | `1.469x` | wall neutral, CPU regression |
| repaint | `0.985x` | `1.006x` | regression |
| scroll_ascii | `0.981x` | `1.011x` | regression |

## Decision

Rejected and reverted.

Despite removing a seemingly redundant GL state call, the benchmark regressed
repaint and scroll wall-time and significantly regressed cursor CPU.  Do not keep.

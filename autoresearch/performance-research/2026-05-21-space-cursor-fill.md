# Experiment: direct block cursor fill for blank cells

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

For a focused block cursor over a blank cell, `gpudrawcursor()` can preserve the
same visual result by drawing only the cursor-colored rectangle instead of calling
`gpudrawcell()` on a space glyph.  The cursor-update benchmark frequently leaves
the cursor on the blank cell after the written character, so this could reduce
cursor overlay overhead without changing visible output.

## Patch tested

In `render/gpu.c`:

- in the block-cursor cases (`0`, `1`, `2`, excluding snowman cursor `7` after it
  changes the rune), if the cursor glyph is a plain space without underline or
  strikethrough, batch only the filled cursor rectangle;
- otherwise keep the existing `gpudrawcell()` behavior.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output: `/tmp/st-ab-spacecursor-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0006x` | `1.1092x` | wall neutral, CPU regression |
| repaint | `1.0073x` | `1.0053x` | tiny wall win, CPU neutral/slightly worse |
| scroll_ascii | `1.0077x` | `0.9847x` | small win |

## Decision

Rejected and reverted.

The change was visually safe for blank block cursors and helped scroll slightly,
but it did not materially improve the cursor blocker and regressed cursor CPU by
about 10%.  That is not acceptable for the current goal of tasteful CPU/wall
improvements.

Post-revert validation:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.  The working tree was clean and the installed binary still matched
the clean repo build.

# Experiment: hot glyph lookup cache

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

The larger Option C baseline still shows repaint and cursor-update wall-time
blockers.  The GPU draw path calls `gpuglyph()` for every non-space cell.  A small
direct-mapped hot cache for recently used non-ASCII `(Rune, flags) -> glyph index`
lookups might reduce hash/probe overhead in repaint and Unicode-heavy rows without
changing visuals or idle behavior.

## Patch tested

In `render/gpu.c`:

- added 64-entry hot glyph arrays to `Gpu`,
- cleared them in `gpuatlasreset()`,
- checked the hot slot before the existing non-ASCII glyph hash table,
- populated the hot slot on hash hits and new glyph insertion.

This was intentionally read-only with respect to rendered output: it only changed
how cached glyphs are found.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output: `/tmp/st-ab-hotglyph-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.983x` | `0.990x` | wall regression, tiny CPU win |
| repaint | `0.996x` | `1.010x` | tiny wall/CPU regression |
| scroll_ascii | `0.976x` | `1.011x` | regression |

## Decision

Rejected and reverted.

The extra cache did not help the measured bottlenecks and regressed ASCII scroll,
which is one of the workloads GPU currently wins.  Do not retry this direct-mapped
hot glyph cache in the same form.

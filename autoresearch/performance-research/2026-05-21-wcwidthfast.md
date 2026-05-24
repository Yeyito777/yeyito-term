# Experiment: approximate `wcwidth()` fast path

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

The repaint profile for the Option C renderer pass showed parser/write-side cost
rather than GPU draw cost dominating the workload.  In that profile, `wcwidth()`
was visible enough to test whether a small hand-written fast path for common
Unicode ranges could reduce repaint/parser overhead without touching rendering.

## Patch tested

In `st.c`:

- added `wcwidthfast(Rune)`,
- returned width 1 for common Latin/Greek/punctuation-ish ranges:
  - `0x00a0..0x02ff`,
  - `0x0370..0x052f`,
  - `0x2014`,
  - `0x2713`,
- returned width 2 for broad CJK/Hangul ranges:
  - `0x2e80..0xa4cf`,
  - `0xac00..0xd7a3`,
- fell back to libc `wcwidth()` otherwise,
- replaced the width lookup in `tputc()` with `wcwidthfast()`.

This was intended as a parser-side optimization only; no visual behavior change
was intended.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test
```

Result: passed with the patch applied.

After rejection and revert:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-wcwidthfast-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0115x` | `0.9962x` | tiny cursor win |
| repaint | `0.9924x` | `0.9999x` | wall regression |
| scroll_ascii | `0.9714x` | `0.8157x` | CPU win, wall regression |

## Decision

Rejected and reverted.

The patch gave a tiny cursor wall/CPU improvement and a scroll CPU improvement,
but it regressed repaint wall time and ASCII scroll wall time.  Because repaint
and scroll wall time are part of the target and the width approximation also adds
Unicode-behavior risk, this is not worth keeping.

Do not retry this approximation in the same form.  If width lookup remains
interesting, prefer exact caching or workload-specific profiling with stronger
wall-time evidence.

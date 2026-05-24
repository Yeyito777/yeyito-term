# Failed experiment: single-entry `wcwidth()` hot cache

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `475c898 Log failed direct cursor advance experiment`

## Hypothesis

The repaint workload repeats a small set of Unicode runes many times per frame,
and profiles still show `wcwidth()` in the write path.  A previous direct-mapped
cache was too broad and regressed CPU.  This experiment tried a much smaller
single-entry hot cache, hoping to catch adjacent repeated runes with minimal
state and lower overhead.

## Patch tested

- Added one cached `Rune` and one cached width in `st.c`.
- Replaced the non-control non-ASCII `wcwidth(u)` call in `tputc()` with a
  `wcwidthhot(u)` helper.

## Validation while patched

```sh
make
make test_gpu_regressions
```

Both passed.

## A/B benchmark

Output: `/tmp/st-ab-wcwidthhot-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 0.9916 | 1.0143 | regressed |
| repaint | 0.9893 | 1.0061 | regressed |
| scroll_ascii | 1.0079 | 0.9974 | slight wall win |

## Decision

Rejected.

The small cache still regressed both cursor and repaint, which are the primary
blockers.  It only helped ASCII scroll slightly, where `wcwidth()` should not be
important.  The patch was reverted and this note is kept to avoid repeating this
cache shape.

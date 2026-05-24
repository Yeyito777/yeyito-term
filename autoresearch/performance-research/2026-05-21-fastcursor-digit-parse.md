# Failed experiment: bounded digit parser in `tfastcursorop`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `d47867d Log failed cursor cell active cache experiment`

## Hypothesis

The GPU cursor update fast path parses row/column numbers with `MIN(9999, ...)` per digit.
Cursor benchmark coordinates are small, so a bounded four-digit parser could avoid the saturating
`MIN` work while safely falling back for oversized CSI coordinates.

## Validation while patched

- `make`
- `make test_gpu_regressions`

Both passed.

## A/B result

Output: `/tmp/st-ab-fastcursorparse-two.jsonl`

| Workload | Wall speedup | CPU ratio |
| --- | ---: | ---: |
| cursor_updates | 0.9967x | 0.9998x |
| repaint | 1.0305x | 1.4224x |
| scroll_ascii | 1.0083x | 0.9578x |

## Decision

Rejected.  Cursor updates still regressed slightly on wall time, and repaint CPU regressed badly.
The wall wins on repaint/ASCII are not worth the CPU regression or the extra fast-path complexity.

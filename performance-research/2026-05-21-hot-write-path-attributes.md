# Failed experiment: GCC `hot` attributes for write-path functions

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `5fc8341 Log failed single-entry wcwidth cache experiment`

## Hypothesis

The parser/write path dominates the repaint profile.  GCC's `hot` function
attribute may improve code layout for the hottest write-side functions without
changing behavior.

## Patch tested

Added a `hot` macro wrapping `__attribute__((hot))` and annotated:

- `twrite()`
- `tputc()`
- `tputcfastascii()`

## Validation while patched

```sh
make
make test_gpu_regressions
```

Both passed.

## A/B benchmark

Output: `/tmp/st-ab-hotattr-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 1.0024 | 0.9860 | tiny positive |
| repaint | 1.0036 | 1.4737 | severe CPU regression |
| scroll_ascii | 1.0120 | 0.9849 | positive |

## Decision

Rejected.

The wall-time numbers looked mildly positive, but repaint CPU regressed badly.
Since repaint CPU is one of the remaining blockers, the change is not acceptable.
The code was reverted and this note is kept to avoid retrying this attribute mix.

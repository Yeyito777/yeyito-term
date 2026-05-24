# Failed experiment: direct printable cursor advance in `tputc()`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `dc28eae Defer Unicode encoding in tputc`

## Hypothesis

After `tputc()` writes a printable glyph, the common non-wrap path used:

```c
tmoveto(term.c.x + width, term.c.y);
```

At that point the target column is known to be in bounds.  Replacing the helper
call with direct cursor-state updates could avoid a hot function call and clamp
work in repaint/scroll workloads:

```c
term.c.state &= ~CURSOR_WRAPNEXT;
term.c.x += width;
```

## Validation while patched

```sh
make
make test_gpu_regressions
```

Both passed.

## A/B benchmark

Output: `/tmp/st-ab-directadvance-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 1.0036 | 1.0048 | tiny wall win, CPU worse |
| repaint | 0.9875 | 1.0177 | regressed |
| scroll_ascii | 1.0168 | 0.9776 | improved |

## Decision

Rejected.

Although ASCII scroll improved, repaint regressed in both wall time and CPU.
Repaint remains one of the main blockers, so this tradeoff is not acceptable.
The code change was reverted and this note is kept to avoid retrying the same
micro-optimization.

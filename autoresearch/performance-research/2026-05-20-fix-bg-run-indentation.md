# Cleanup: fix GPU background-run indentation

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Context

While reviewing `render/gpu.c` for additional performance experiments, the
background-run transition block in `gpudrawline()` had misleading indentation:

- `runw = cellw;`
- `memcpy(runbg, bg, sizeof runbg);`
- the closing brace

were over-indented.  The generated C behavior was correct, but the indentation made
that branch harder to audit.

## Change

Fixed indentation only.  No functional or performance change intended.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
git -C ~/Config/st diff --check
```

Result: passed.

## Performance result

No runtime benchmark required; this is formatting-only cleanup in the renderer hot
path, intended to reduce future-review risk without changing compiled behavior.

## Decision

Keep and commit as no-regression cleanup.

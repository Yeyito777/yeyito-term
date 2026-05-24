# Failed experiment: branch hints inside `tfastcursorop()`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `ee2bf75 Log failed semantic interposition experiment`

## Hypothesis

The cursor-update workload heavily exercises `tfastcursorop()`, which recognizes
sequences of the form:

```text
CSI row;col H  CSI 3xm  ASCII char  CSI 0m
```

This experiment marked the rejection/error branches in that recognizer with
`unlikely(...)`, hoping to improve layout for the successful cursor-update fast
path without changing behavior.

## Validation while patched

```sh
make
make test_gpu_regressions
```

Both passed.

## A/B benchmark

Output: `/tmp/st-ab-cursorhint-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 1.0008 | 1.0110 | wall noise, CPU slightly worse |
| repaint | 1.0080 | 1.0014 | tiny wall improvement, neutral CPU |
| scroll_ascii | 0.9975 | 1.0038 | slight regression |

## Decision

Rejected.

The only positive result was a very small repaint wall-time improvement, while
cursor CPU and ASCII scroll both regressed slightly.  Since cursor updates are a
primary blocker and the effect size is tiny, this is not worth keeping.

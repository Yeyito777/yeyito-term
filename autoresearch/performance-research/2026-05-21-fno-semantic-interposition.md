# Failed experiment: `-fno-semantic-interposition`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `5e79b82 Log failed call-site branch hint experiment`

## Hypothesis

Try adding `-fno-semantic-interposition` to the optimized build flags:

```make
CFLAGS = -O3 -march=native -flto -fno-semantic-interposition
```

The hope was that this would give GCC more freedom to optimize calls and symbol
references in the hot parser/render paths without touching runtime behavior.

## Validation while patched

The patched tree built and passed targeted GPU regression tests:

```sh
make clean
make
make test_gpu_regressions
```

## A/B benchmark

Output: `/tmp/st-ab-nosemantic-two.jsonl`

| Workload | Wall speedup | CPU ratio | Decision |
|---|---:|---:|---|
| cursor_updates | 1.0029 | 0.9968 | neutral/tiny |
| repaint | 0.9952 | 1.0354 | regressed |
| scroll_ascii | 0.9979 | 1.0052 | regressed |

## Decision

Rejected.

The cursor result was effectively noise and the patch regressed repaint and
ASCII scroll in both wall time and CPU.  The code change was reverted and this
note is kept to avoid retrying the build flag in the same form.

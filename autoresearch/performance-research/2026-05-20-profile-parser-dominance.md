# Profiling checkpoint: parser/write path dominates repaint and cursor tests

Date: 2026-05-20
Branch: `gpu-renderer-prototype`
Tip at profiling: around `b3cafe2`/`4cf826d` research log commits; source code clean for accepted renderer.

## Purpose

Record profiling evidence to guide future experiments.  This is not a code change.

## Method

Built current GPU source and profiled long synthetic workloads under `xenv` with
`perf record --no-inherit -F 999 --call-graph fp` so the child Python workload does
not dominate samples.

Cursor profile command shape:

```sh
DISPLAY=$disp perf record --no-inherit -F 999 --call-graph fp \
  -o /tmp/st-prof-long-cursor.perf -- \
  /tmp/st-prof-long-gpu -T prof-long -e sh -c \
  'sleep 0.2; /tmp/st-render-workload.py cursor_updates 1000000 || true'
```

Repaint profile command shape:

```sh
DISPLAY=$disp perf record --no-inherit -F 999 --call-graph fp \
  -o /tmp/st-prof-long-repaint.perf -- \
  /tmp/st-prof-repaint-gpu -T prof-repaint -e sh -c \
  'sleep 0.2; /tmp/st-render-workload.py repaint 5000 || true'
```

## Findings

### Cursor workload

Representative no-children sample shares:

| Symbol / area | Approx. share | Notes |
|---|---:|---|
| `draw` | `14.0%` | renderer/draw frame cost still meaningful |
| `twrite` | `5.6%` | parser/write loop |
| `tputcfastascii` | `4.6%` | fast ASCII cell writer |
| startup/resize/GL/XIM noise | visible but not workload-only | launch profile still includes some initialization despite sleep |

### Repaint workload

Representative no-children sample shares:

| Symbol / area | Approx. share | Notes |
|---|---:|---|
| `tputcfastascii` | `29.3%` | ASCII-heavy part of repaint dominates |
| `twrite` | `14.8%` | parser/write loop |
| `tputc` | `9.0%` | Unicode path for repaint text |
| `wcwidth` | `6.8%` | Unicode width lookup is visible |
| `draw` | `0.7%` | repaint profile here is parser-heavy, not draw-heavy |

## Interpretation

The remaining repaint/cursor blockers are not purely GPU draw-call overhead.  The
write/parser path is a large fraction, especially for repaint.  This explains why
several GL-state/batch experiments failed to move the needle or improved one metric
while regressing another.

Promising future directions should either:

- reduce parser/write overhead without hurting scroll/cursor wall time, or
- change workload scheduling/batching at a higher level,

rather than only micro-tuning GL state.

## Decision

Keep as a research checkpoint.  No source behavior changed.

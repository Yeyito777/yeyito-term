# Experiment: skip foreground color resolution for plain spaces

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

`gpudrawline()` resolves both foreground and background for every cell.  For a
plain space with no underline/strikethrough, the foreground is not used.  Avoiding
foreground resolution for default-background plain spaces might reduce CPU in
cursor/repaint/scroll workloads, especially where whole dirty rows contain many
spaces.

## Patch tested

In `render/gpu.c`, inside the common default-background fast path, copy the
background color immediately but only resolve/copy the foreground when the cell
will actually draw text or decoration:

- `g.u != ' '`, or
- `ATTR_UNDERLINE|ATTR_STRUCK` is set.

## Validation

Commands run before benchmarking:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Temporary workload/benchmark scripts were recreated under `/tmp` for this run.
Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.0029x` | `1.004x` | neutral/slightly worse CPU |
| repaint | `0.9848x` | `1.533x` | regression |
| scroll_ascii | `1.0086x` | `1.291x` | wall tiny positive, CPU regression |

## Decision

Rejected and reverted.

The change produced at best a tiny cursor/scroll wall improvement, but repaint and
scroll CPU regressed significantly.  Do not retry this exact form without a more
selective trigger or stronger evidence.

# Failed experiment: guard selection_active() with selection state

## Hypothesis

The GPU renderer calls `selection_active()` once per row and for cursor overlay
cells. In the benchmark's normal no-selection state, a cheaper direct check of the
selection mode before calling `selection_active()` might avoid some hot-path work.

## Patch attempt

The experiment tried to change GPU renderer code from:

```c
int selactive = selection_active();
```

to:

```c
int selactive = sel.mode != SEL_EMPTY && selection_active();
```

in `gpudrawline()`, `gpudrawcell()`, and `gpudrawcursor()`.

## Result

Build failed:

```text
render/gpu.c:818:25: error: 'sel' undeclared (first use in this function)
render/gpu.c:908:25: error: 'sel' undeclared (first use in this function)
render/gpu.c:950:25: error: 'sel' undeclared (first use in this function)
```

`render/gpu.c` is included from `x.c`, where the selection object is not directly
available under that name. Exposing or duplicating selection internals just to
avoid this helper call would add API coupling and risk for a tiny optimization.

## Decision

Rejected and reverted before benchmarking.

Do not retry this direct `sel.mode` guard unless the selection API is deliberately
changed for a broader reason.

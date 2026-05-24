# Remove disabled VBO renderer path

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

The GPU renderer still carried an old streaming-VBO implementation behind `if (0)`.
Prior benchmark rounds showed the VBO path was slower/noisier than client arrays for
our Xephyr/small-damage workloads.  Removing the disabled code should not change
runtime behavior, but should improve code quality and avoid misleading future
experiments.

## Change

Removed dead VBO-specific state and code from `render/gpu.c`:

- unused GL buffer function pointer typedefs,
- unused VBO fields in `Gpu`,
- disabled `if (0)` initialization block,
- unreachable VBO upload/destroy branches in `gpudrawbatch()`/`gpudestroy()`.

## Validation

Commands run:

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## Performance result

No runtime performance benchmark needed for the code path itself because the
removed implementation was disabled (`if (0)`) and therefore unreachable.  This is
classified as a cleanup-only performance research result: it removes a known failed
renderer path and reduces maintenance risk without changing behavior.

## Decision

Keep if full tests continue to pass.  This should be committed as cleanup, not as
a measured runtime speedup.

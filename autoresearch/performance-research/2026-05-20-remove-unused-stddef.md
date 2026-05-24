# Cleanup: remove unused `<stddef.h>` include

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Context

After removing the disabled VBO renderer path, `x.c` no longer uses `offsetof()`
or any other symbol from `<stddef.h>`.

## Change

Removed the now-unused include:

```c
#include <stddef.h>
```

## Validation

Commands run:

```sh
make -C ~/Config/st
make -C ~/Config/st test
make -C ~/Config/st test_gpu_regressions
git -C ~/Config/st diff --check
```

Result: passed.

## Performance result

No runtime behavior or benchmark change is expected.  This is a cleanup-only result
following the VBO removal: it reduces stale dependencies and keeps the renderer
cleanup branch tidy without affecting generated renderer behavior.

## Decision

Keep and commit as no-regression cleanup.

# Failed experiment: reserve full-screen text batch capacity

## Hypothesis

Large repaint and scrolling frames can push thousands of glyph vertices through
`gpu.text` / `gpu.ctext`. The accepted growth policy starts batches at the requested
vertex count plus 2048 slack, so a first full-screen text frame can still trigger
multiple reallocations and copies. Reserving enough capacity for roughly one full
screen of glyph triangles when the main text or color-text batch is first used
could reduce startup/frame overhead while leaving overlay and solid batches on the
accepted policy.

## Patch summary

In `render/gpu.c`, `gpubatchalloc()` computed the accepted `need` value:

```c
int need = b->len + n + 2048;
```

and, only for initially empty main text/color-text batches, raised it to about one
full terminal screen of glyph triangles:

```c
if (b->cap == 0 && (b == &gpu.text || b == &gpu.ctext) &&
    win.cw > 0 && win.tw > 0 && trow() > 0)
    need = MAX(need, (win.tw / win.cw) * trow() * 6);
```

The final growth rule remained `MAX(b->cap * 2, need)`. Rendering semantics,
actual GPU path, triangle batches, glyph/emoji behavior, fractional scaling,
accepted clear-color cache, cleared-background skip, and accepted vimnav row guard
were otherwise unchanged. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed after an initial implementation typo was corrected to use `win`/`trow()`
instead of the unavailable internal `term` symbol.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/screen-text-reserve/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/screen-text-reserve-validate/result.json`

## Validation result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment validation score: `0.859840`  
Relative score: `0.988x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8386 | 1.6343 | 1.8470 | 0.999x | 0.998x | 0.997x |
| repaint | 0.8133 | 1.2574 | 1.8301 | 0.961x | 1.048x | 1.000x |
| scroll_ascii | 0.9688 | 1.0111 | 1.8657 | 0.983x | 1.026x | 1.000x |
| scroll_unicode | 0.9306 | 1.0874 | 1.8284 | 0.978x | 1.042x | 0.999x |
| scroll_emoji | 1.1752 | 0.7333 | 1.8701 | 1.036x | 0.950x | 0.999x |

## Decision

Rejected and reverted.

The initial 7-iteration run was barely positive (`0.870976`, `1.0008x` accepted)
with repaint and ASCII gains, but the 9-iteration validation did not reproduce it.
Validation regressed repaint, ASCII, and unicode enough to fall well below
accepted. The accepted lazy batch growth policy remains better.

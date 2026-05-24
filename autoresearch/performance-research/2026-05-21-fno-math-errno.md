# Experiment: `-fno-math-errno`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`

## Hypothesis

The GPU renderer calls math helpers such as `floor()`/`fabs()` in geometry and
resize paths.  Since st does not inspect `errno` after math calls, compiling with
`-fno-math-errno` might let the compiler emit cheaper code without changing
observable terminal behavior.

## Patch tested

In `config.mk`:

```make
CFLAGS = -O3 -march=native -flto -fno-math-errno
```

Because this is a compiler-flag experiment, both current and baseline binaries
were rebuilt from clean objects during A/B testing.

## Validation

```sh
make clean
make
make test_gpu_regressions
```

Result: passed with the patch applied.

After rejection and revert:

```sh
make clean
make
make test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Benchmark output was saved to `/tmp/st-ab-fnomatherrno-two.jsonl`.

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `0.9971x` | `0.9820x` | tiny CPU win, wall regression/noise |
| repaint | `1.0022x` | `0.6904x` | good CPU result in A/B |
| scroll_ascii | `0.9978x` | `0.9783x` | tiny CPU win, wall regression/noise |

## Fair GPU-vs-Xft benchmark

Benchmark output was saved to `/tmp/st-quick-bench-fair-fnomatherrno.jsonl`.

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft |
|---|---:|---:|
| scroll_ascii | `1.0536x` | `0.7156x` |
| scroll_unicode | `0.9820x` | `0.7983x` |
| scroll_emoji | `1.2354x` | `0.6883x` |
| repaint | `0.9301x` | `0.6872x` |
| cursor_updates | `0.8324x` | `1.2037x` |

## Decision

Rejected and reverted.

The A/B CPU numbers looked interesting, especially for repaint, but wall time was
neutral/regressive in cursor and ASCII scroll.  The fair benchmark still showed
both remaining blockers losing in wall time, and cursor CPU remained worse than
Xft.  Because the flag is global and does not produce a clear all-around win, it
is not worth keeping.

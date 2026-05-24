# Experiment: direct-mapped `wcwidth()` cache

Date: 2026-05-20
Branch: `gpu-renderer-prototype`

## Hypothesis

A long repaint profile showed parser/write-side cost dominating GPU repaint, with
`wcwidth()` appearing in the profile for Unicode content.  A small direct-mapped
cache keyed by `Rune` might reduce repeated width lookups in Unicode repaint and
scroll workloads.

## Patch tested

In `st.c`:

- added a 4096-entry direct-mapped cache:
  - `wcwidth_cache_rune[]`,
  - `wcwidth_cache_width[]`,
- added `wcwidthcached(Rune)`,
- changed `tputc()` to call `wcwidthcached(u)` instead of `wcwidth(u)` for
  non-ASCII UTF-8 runes.

The cache is safe under the assumption that locale/wcwidth semantics are stable
after startup, which is true for current st usage.

## Validation

```sh
make -C ~/Config/st
make -C ~/Config/st test_gpu_regressions
```

Result: passed.

## A/B benchmark vs current GPU baseline

Median kept-iteration results:

| Workload | Wall speedup | CPU ratio | Result |
|---|---:|---:|---|
| cursor_updates | `1.014x` | `1.001x` | tiny cursor wall win |
| repaint | `1.017x` | `1.454x` | wall win but CPU regression |
| scroll_ascii | `0.971x` | `0.801x` | CPU win but wall regression |

## Decision

Rejected and reverted.

The result was mixed/noisy: cursor and repaint wall improved in this run, but
repaint CPU regressed badly and ASCII scroll wall regressed.  Since the change adds
global parser state and does not provide an unambiguous no-regression win, do not
keep.

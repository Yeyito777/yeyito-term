# Optimization: lazily encode Unicode in `tputc()`

Date: 2026-05-21
Branch: `gpu-renderer-prototype`
Base: `ee2bf75 Log failed semantic interposition experiment`

## Hypothesis

The repaint profile still showed the write side dominating runtime, including
`tputc()`, `wcwidth()`, and related UTF-8 handling.  For normal printable
Unicode, `tputc()` only needs the decoded `Rune` and its display width; the
encoded byte sequence is only needed for printer mode or string escape sequence
buffering.

Previously `tputc()` encoded every non-ASCII rune immediately:

```c
len = utf8encode(u, c);
```

This experiment defers that encoding until it is actually needed by `MODE_PRINT`
or `ESC_STR` handling.

## Safety notes

The patch keeps behavior for the paths that need encoded bytes:

- `MODE_PRINT` encodes before `tprinter()`.
- `ESC_STR` encodes before the string buffer capacity check and copy.
- Normal printable glyph handling continues to use the original `Rune` and
  `wcwidth()` result.

## Validation

The patched tree passed:

```sh
make
make test
make test_gpu_regressions
```

## A/B benchmark against current GPU baseline

Output: `/tmp/st-ab-lazyutf8-two.jsonl`

| Workload | Wall speedup | CPU ratio | Notes |
|---|---:|---:|---|
| cursor_updates | 1.0033 | 0.9981 | effectively neutral/slightly positive |
| repaint | 1.0143 | 0.9602 | useful win on the Unicode repaint workload |
| scroll_ascii | 0.9992 | 0.9846 | neutral wall, slight CPU improvement |

## Fair GPU-vs-Xft snapshot while patched

Output: `/tmp/st-quick-bench-fair-lazyutf8.jsonl`

| Workload | GPU wall speedup vs Xft | GPU CPU ratio vs Xft |
|---|---:|---:|
| scroll_ascii | 1.0653 | 1.1465 |
| scroll_unicode | 1.0149 | 0.9649 |
| scroll_emoji | 1.2153 | 0.6774 |
| repaint | 0.9415 | 1.0506 |
| cursor_updates | 0.8385 | 0.7906 |

The fair benchmark remains noisy, especially for CPU ratios, but the direct GPU
A/B result is positive on repaint without meaningful regressions in the quick
coverage set.

## Decision

Kept.

This is a small write-path cleanup that removes avoidable Unicode work from the
normal printable path.  It improves the targeted repaint A/B workload and does
not alter rendering behavior.

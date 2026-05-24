# Retraction: cleared empty-row skip based on `tlinelen()`

## Retracted change

Commit `cdeff23` added an early return in `gpudrawline()` on fully-cleared frames
when `tlinelen(y) == 0`.

The benchmark validation for that change was positive:

- previous accepted score: `0.863260`
- `tlinelen()` empty-row validation score: `0.875049`

## Correctness issue

`tlinelen(y) == 0` only proves the row contains no non-space characters according
to st's text-length helper. It does **not** prove the row is visually empty for
the GPU renderer. For example, a row of spaces with custom background colors,
underline/strike attributes, or other cell-local visual attributes can still be
visible even though `tlinelen()` returns zero.

Skipping such a row after `glClear()` would incorrectly erase those visible
space-cell effects. That violates the autoresearch goal's requirement to preserve
GPU-renderer behavior and features.

## Follow-up experiment

A correctness-preserving version that scanned every cell for ordinary default
spaces was tested and logged separately:

- `autoresearch/gpu-cpu-llvmpipe-performance/2026-05-24-failed-correct-empty-row-skip.md`
- result: `autoresearch/gpu-cpu-llvmpipe-performance/runs/correct-empty-row-skip/result.json`
- score: `0.859259`

That was slower than the prior accepted cleared-background-only state.

## Decision

Retract the `tlinelen()` empty-row optimization from the live code. The accepted
live optimization frontier returns to the behavior-preserving cleared default
background skip:

- `5f2e9f8 Skip cleared default GPU backgrounds under llvmpipe research`
- accepted score: `0.863260`

This keeps the actual GPU renderer path intact and avoids a correctness bug. It
does not fallback to Xft, disable `gpudraw`, or switch renderers under llvmpipe.

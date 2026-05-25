# Failed experiment: skip fallback char-index check for ASCII glyphs

## Hypothesis

ASCII glyphs should be present in the primary terminal font. In `gpuglyph()`, the
renderer still calls `FT_Get_Char_Index()` and may run the fallback lookup path for
all runes. Skipping that fallback-presence check for `rune < 128` could reduce
first-use ASCII glyph upload overhead while preserving normal ASCII rendering.

## Patch summary

In `render/gpu.c`, the experiment changed:

```c
if (!FT_Get_Char_Index(face, rune)) {
```

to:

```c
if (rune >= 128 && !FT_Get_Char_Index(face, rune)) {
```

This preserved the actual GPU renderer path, glyph rasterization, fractional
scaling, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, cleared-background skip, and accepted vimnav row guard. It did
not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/ascii-no-fallback-check/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.862315`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8316 | 1.6661 | 1.8504 | 0.991x | 1.017x | 0.999x |
| repaint | 0.8421 | 1.2052 | 1.8323 | 0.995x | 1.004x | 1.001x |
| scroll_ascii | 0.9598 | 0.9965 | 1.8656 | 0.974x | 1.011x | 1.000x |
| scroll_unicode | 0.9287 | 1.0708 | 1.8291 | 0.976x | 1.026x | 1.000x |
| scroll_emoji | 1.1467 | 0.7601 | 1.8695 | 1.011x | 0.985x | 0.999x |

## Decision

Rejected and reverted.

The change helped emoji wall slightly, likely from noise or different fallback
behavior, but it regressed cursor, ASCII, and unicode wall ratios and lowered the
weighted score. Keep the conservative fallback char-index check for all runes.

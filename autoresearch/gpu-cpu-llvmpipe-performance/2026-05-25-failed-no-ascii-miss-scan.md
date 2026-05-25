# Failed experiment: skip linear glyph scan on ASCII cache miss

## Hypothesis

The GPU renderer has a direct `gpu.ascii[style][rune]` cache for ASCII glyphs. If
that cache entry is `-1`, the accepted code still linearly scans the glyph vector
for the ASCII rune/style before creating a new glyph. Since atlas reset clears both
`gpu.ascii` and `gpu.glyphlen`, and each loaded ASCII glyph stores its index in
`gpu.ascii`, the linear miss scan should be redundant. Removing it could reduce
first-use ASCII glyph overhead.

## Patch summary

In `render/gpu.c`, the experiment removed the `rune < 128` linear scan fallback in
`gpuglyph()` after an ASCII cache miss. ASCII cache hits and non-ASCII hash table
lookup were unchanged, and new ASCII glyphs still populated `gpu.ascii`.

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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/no-ascii-miss-scan/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.856526`  
Relative score: `0.984x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8237 | 1.6526 | 1.8522 | 0.981x | 1.009x | 1.000x |
| repaint | 0.8180 | 1.2411 | 1.8312 | 0.966x | 1.034x | 1.001x |
| scroll_ascii | 0.9852 | 0.9794 | 1.8663 | 1.000x | 0.993x | 1.001x |
| scroll_unicode | 0.9438 | 1.0695 | 1.8279 | 0.992x | 1.024x | 0.999x |
| scroll_emoji | 1.1292 | 0.7744 | 1.8695 | 0.996x | 1.004x | 0.999x |

## Decision

Rejected and reverted.

Although the scan appears redundant, removing it did not improve the benchmark and
regressed cursor/repaint wall time significantly. Keep the accepted conservative
ASCII miss path.

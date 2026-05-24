# Failure: cache glyph texture coordinates

Date: 2026-05-24
Branch: `autoresearch-gpu-cpu-llvmpipe`

## Hypothesis

Avoid per-glyph texture-coordinate division during batching by computing atlas texture coordinates once when a glyph is uploaded.  This preserves the GPU renderer and fractional scaling because only texture-coordinate bookkeeping changes.

## Patch summary

- Added `tx1/ty1/tx2/ty2` to `GpuGlyph`.
- Computed texture coordinates at glyph upload time.
- Changed `gpubatchglyph()` to use cached coordinates.

## Benchmark

Benchmark: `autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py`
Run: `runs/cached-glyph-texcoords/result.json`

Current accepted lazy-color-atlas score: `0.709839`
Experiment score: `0.704361`
Experiment vs accepted multiplier: `0.992x`

| Workload | Accepted wall speedup | Experiment wall speedup | Accepted CPU ratio | Experiment CPU ratio |
| --- | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5810 | 0.5872 | 2.8366 | 2.8101 |
| repaint | 0.7063 | 0.6985 | 1.5396 | 1.5758 |
| scroll_ascii | 0.9009 | 0.8879 | 1.1210 | 1.1460 |
| scroll_unicode | 0.8617 | 0.8529 | 1.2014 | 1.2196 |
| scroll_emoji | 1.0647 | 1.0491 | 0.8574 | 0.8692 |

## Decision

Rejected and reverted.  The experiment was still better than the original baseline, but it regressed the accepted lazy-color-atlas state by about 0.8% and did not improve repaint.  The extra glyph-struct size/cache footprint likely outweighed the saved divisions under llvmpipe.

## Validation

- `make`
- `make test_gpu_regressions`
- benchmark run: `runs/cached-glyph-texcoords/result.json`
- failed code reverted before this log commit

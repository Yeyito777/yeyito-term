# Failure: lazy main glyph atlas allocation

Date: 2026-05-24
Branch: `autoresearch-gpu-cpu-llvmpipe`

## Hypothesis

After accepting lazy color-atlas allocation, also delay main alpha atlas storage allocation until the first non-color glyph upload.  This might reduce first-frame/startup CPU/RSS for llvmpipe while preserving the real GPU renderer codepath.

## Patch summary

- Added `gpu.atlasready`.
- Changed `gpuatlasreset()` to skip immediate `glTexImage2D()` for the main alpha atlas.
- Allocated alpha atlas storage lazily before first normal glyph `glTexSubImage2D()`.

## Benchmark

Benchmark: `autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py`
Run: `runs/lazy-main-and-color-atlas/result.json`

Original baseline score: `0.681802`
Current accepted lazy-color-atlas score: `0.709839`
Experiment score: `0.699824`
Experiment vs accepted multiplier: `0.986x`

| Workload | Accepted wall speedup | Experiment wall speedup | Accepted CPU ratio | Experiment CPU ratio |
| --- | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5810 | 0.5786 | 2.8366 | 2.8554 |
| repaint | 0.7063 | 0.6996 | 1.5396 | 1.5639 |
| scroll_ascii | 0.9009 | 0.8606 | 1.1210 | 1.1668 |
| scroll_unicode | 0.8617 | 0.8595 | 1.2014 | 1.2124 |
| scroll_emoji | 1.0647 | 1.0539 | 0.8574 | 0.8698 |

## Decision

Rejected and reverted.  The experiment remained above the original baseline, but it regressed the already-accepted lazy-color-atlas state by about 1.4% weighted score and materially hurt `repaint`, `scroll_ascii`, and `scroll_unicode`.  Main glyphs are needed immediately by all benchmark workloads, so moving the alpha atlas allocation from reset to first glyph upload only shifts cost into the hot first draw instead of removing it.

## Validation

- `make`
- `make test_gpu_regressions`
- benchmark run: `runs/lazy-main-and-color-atlas/result.json`
- failed code reverted before this log commit

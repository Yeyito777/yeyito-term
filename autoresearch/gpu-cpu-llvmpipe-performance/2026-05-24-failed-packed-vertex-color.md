# Failure: packed unsigned-byte vertex colors

Date: 2026-05-24
Branch: `autoresearch-gpu-cpu-llvmpipe`

## Hypothesis

Reduce CPU memory bandwidth and llvmpipe vertex processing cost by storing per-vertex colors as normalized unsigned bytes instead of four floats, while preserving positions and texture coordinates as floats for fractional scaling.

## Patch summary

- Changed `GpuVertex.r/g/b/a` from `GLfloat` to `GLubyte`.
- Converted batch color floats to 0-255 bytes in `gpubatchquad()`.
- Changed `glColorPointer()` from `GL_FLOAT` to `GL_UNSIGNED_BYTE`.

## Benchmark

Benchmark: `autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py`
Run: `runs/packed-vertex-color/result.json`

Current accepted lazy-color-atlas score: `0.709839`
Experiment score: `0.699061`
Experiment vs accepted multiplier: `0.985x`

| Workload | Accepted wall speedup | Experiment wall speedup | Accepted CPU ratio | Experiment CPU ratio |
| --- | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5810 | 0.5817 | 2.8366 | 2.8895 |
| repaint | 0.7063 | 0.6898 | 1.5396 | 1.5962 |
| scroll_ascii | 0.9009 | 0.9008 | 1.1210 | 1.1275 |
| scroll_unicode | 0.8617 | 0.8490 | 1.2014 | 1.2190 |
| scroll_emoji | 1.0647 | 1.0263 | 0.8574 | 0.8923 |

## Decision

Rejected and reverted.  Although still above the original baseline, the packed-color layout regressed the accepted lazy-color-atlas score by about 1.5%, including worse `repaint`, `scroll_unicode`, and `scroll_emoji`.  The likely cost is per-vertex color conversion/normalization in llvmpipe outweighing reduced client-array bandwidth.

## Validation

- `make`
- `make test_gpu_regressions`
- benchmark run: `runs/packed-vertex-color/result.json`
- failed code reverted before this log commit

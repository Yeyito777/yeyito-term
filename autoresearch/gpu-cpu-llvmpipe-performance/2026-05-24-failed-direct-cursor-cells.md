# Failure: direct dirty-cell drawing for fast cursor updates

Date: 2026-05-24
Branch: `autoresearch-gpu-cpu-llvmpipe`

## Hypothesis

The cursor benchmark changes individual cells but `tfastcursorop()` currently dirties whole rows.  Track fast-cursor dirty cells and draw those cells directly through the GPU renderer so llvmpipe does less per-row work while preserving the GPU path and fractional scaling.

## Patch summary

- Added a GPU cell draw wrapper exposed through `win.h`.
- Added dirty-cell tracking in `st.c` for `tfastcursorop()` writes.
- Drew queued dirty cells after normal dirty-row drawing and before cursor overlays.

## Benchmark

Benchmark: `autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py`
Run: `runs/direct-cursor-cells/result.json`

Current accepted lazy-color-atlas score: `0.709839`
Experiment score: `0.694447`
Experiment vs accepted multiplier: `0.978x`

| Workload | Accepted wall speedup | Experiment wall speedup | Accepted CPU ratio | Experiment CPU ratio |
| --- | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5810 | 0.5687 | 2.8366 | 2.9176 |
| repaint | 0.7063 | 0.6885 | 1.5396 | 1.6211 |
| scroll_ascii | 0.9009 | 0.8722 | 1.1210 | 1.1575 |
| scroll_unicode | 0.8617 | 0.8611 | 1.2014 | 1.2089 |
| scroll_emoji | 1.0647 | 1.0547 | 0.8574 | 0.8693 |

## Decision

Rejected and reverted.  The direct-cell path regressed the accepted lazy-color-atlas state by about 2.2% weighted score and did not improve the high-priority `cursor_updates` workload.  The additional bookkeeping/draw overhead outweighed any reduction in row redraw work under llvmpipe.

## Validation

- `make`
- `make test_gpu_regressions`
- benchmark run: `runs/direct-cursor-cells/result.json`
- failed code reverted before this log commit

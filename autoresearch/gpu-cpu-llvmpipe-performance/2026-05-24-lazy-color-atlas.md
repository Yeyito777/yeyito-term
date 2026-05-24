# Success: lazily allocate the color emoji atlas for llvmpipe

Date: 2026-05-24
Branch: `autoresearch-gpu-cpu-llvmpipe`

## Hypothesis

The GPU renderer always allocates the full RGBA color-emoji atlas during `gpuatlasreset()`, even for workloads that never render color emoji.  Under Mesa llvmpipe this is a large CPU/RSS cost paid on startup/resize/first draw.  Delay allocating the color atlas texture storage until the first color glyph upload.  This keeps the GPU renderer and its fractional-scaling/text/emoji behavior intact: color emoji still use the same atlas and filtering once needed, but non-emoji and cursor/repaint workloads do not pay the color-atlas allocation cost.

## Patch summary

- Add `gpu.catlasready` to track whether RGBA color-atlas storage has been allocated.
- In `gpuatlasreset()`, reset `catlasready` but do not call `glTexImage2D()` for `gpu.catlas` immediately.
- In the color glyph upload path, allocate the RGBA atlas on first use before `glTexSubImage2D()`.

## Benchmark

Benchmark: `autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py`
Run: `runs/lazy-color-atlas/result.json`

Baseline total score: `0.681802`
Experiment total score: `0.709839`
Score multiplier: `1.041x`

| Workload | Baseline wall speedup | Experiment wall speedup | Baseline CPU ratio | Experiment CPU ratio | Baseline RSS ratio | Experiment RSS ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5691 | 0.5810 | 2.8940 | 2.8366 | 1.9553 | 1.8707 |
| repaint | 0.6602 | 0.7063 | 1.6866 | 1.5396 | 1.9330 | 1.8476 |
| scroll_ascii | 0.8610 | 0.9009 | 1.1756 | 1.1210 | 1.9636 | 1.8767 |
| scroll_unicode | 0.8538 | 0.8617 | 1.2253 | 1.2014 | 1.9270 | 1.8437 |
| scroll_emoji | 1.0330 | 1.0647 | 0.8758 | 0.8574 | 1.9496 | 1.9554 |

## Decision

Accepted.  The weighted score improved by about 4.1%, with improvements on both high-priority workloads (`cursor_updates` and `repaint`) and no renderer bypass or feature loss.  `scroll_emoji` also improved in this run because allocation is still performed correctly on first emoji use and the measured cost/behavior remains favorable.

## Validation

- `make`
- `make test`
- `make test_gpu_regressions`
- benchmark run: `runs/lazy-color-atlas/result.json`

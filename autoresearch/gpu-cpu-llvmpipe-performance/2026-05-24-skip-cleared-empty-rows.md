# Accepted experiment: skip empty rows after full GPU clear

## Hypothesis

After accepting the full-frame clear plus skipped default-background runs,
completely empty terminal rows on a cleared frame are already represented by the
clear color. The current renderer still walks every cell in those rows, resolves
colors, computes cell geometry, builds a default-background run, and then skips
emitting it at the end. Early-returning for empty rows should avoid that per-cell
CPU work while preserving non-empty rows, overlays, text, emoji, decorations, and
fractional scaling.

## Patch summary

In `render/gpu.c`, `gpudrawline()` now returns early when all of these are true:

- the current frame was fully cleared by the GPU path (`gpu.clearedframe`),
- row-wide effects that could make blank/default cells visible are inactive
  (selection, search matches, reverse video, debug prompt, vimnav current line),
- `tlinelen(y) == 0`.

```c
if (gpu.clearedframe && !selactive && !searchactive &&
    !IS_SET(MODE_REVERSE) && !debug_mode && y != vimline &&
    tlinelen(y) == 0)
    return;
```

The change stays inside the actual GPU renderer path. It does not fallback to
Xft, disable `gpudraw`, detect llvmpipe to switch renderers, or alter GPU text /
emoji rendering behavior. Non-empty rows and all visible row effects still use
the existing renderer.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-empty-rows/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-empty-rows-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-skip-cleared-empty-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-empty-rows-validate
```

## Validation result versus accepted skipped-cleared-background state

Previous accepted score: `0.863260`  
Initial score: `0.871327`  
Validation score: `0.875049`  
Validation relative score: `1.014x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8438 | 1.6631 | 1.8480 | 1.005x | 1.004x | 0.999x |
| repaint | 0.8324 | 1.2250 | 1.8335 | 1.002x | 0.996x | 1.001x |
| scroll_ascii | 0.9830 | 0.9840 | 1.8645 | 1.004x | 0.987x | 0.999x |
| scroll_unicode | 0.9417 | 1.0644 | 1.8291 | 1.006x | 0.993x | 1.001x |
| scroll_emoji | 1.2374 | 0.7091 | 1.8693 | 1.081x | 0.930x | 1.000x |

## Decision

Accepted.

Both benchmark runs improved the previous accepted score, and the validation run
improved every workload wall-time ratio. The high-priority cursor and repaint
workloads both improved slightly, while CPU is neutral-to-better overall and RSS
is effectively flat. The code is also small and behavior-preserving: it only
skips rows that are completely empty and already covered by the full-frame clear.

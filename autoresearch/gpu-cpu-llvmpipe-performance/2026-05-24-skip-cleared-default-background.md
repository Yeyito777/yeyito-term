# Accepted experiment: skip default-background draws after full GPU clear

## Hypothesis

When the GPU path performs a full-frame clear to the terminal default background,
subsequent background quads for runs that resolve to the same default background
are redundant. Under Mesa llvmpipe, those solid quads still consume CPU-side
rasterization and draw-call/batch work. Skipping them after a full clear should
reduce the dominant cursor/repaint cost while preserving all non-default
backgrounds, overlay drawing, text, decorations, fractional scaling, glyphs, and
emoji.

This is especially relevant in the benchmark environment because the GLX/Xephyr
path often requires full clears/redraws to keep double-buffered back-buffer
contents valid.

## Patch summary

In `render/gpu.c`:

- Added `gpu.clearedframe` renderer state.
- In `gpudrawline()`, when closing a background run, skip batching the run if:
  - the current frame was fully cleared, and
  - the run background is exactly the resolved default background color for the
    row.
- Overlay/cursor cell drawing is unchanged and still batches its background.
- Non-default backgrounds (selection, search, vimnav current line, debug prompt,
  reverse-video, true-color/custom backgrounds) are still batched normally.

In `x.c`:

- Reset `gpu.clearedframe = 0` at GPU frame start.
- Set `gpu.clearedframe = 1` immediately after the full-frame `glClear()`.

The change stays entirely inside the actual GPU renderer path. It does not
fallback to Xft, disable `gpudraw`, detect llvmpipe to switch renderers, or alter
text/emoji rendering behavior.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-default-bg/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-default-bg-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-skip-cleared-bg-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/skip-cleared-default-bg-validate
```

## Validation result versus accepted all-textured alpha-test state

Previous accepted score: `0.756262`  
Initial score: `0.863598`  
Validation score: `0.863260`  
Validation relative score: `1.141x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8400 | 1.6562 | 1.8506 | 1.287x | 0.685x | 0.999x |
| repaint | 0.8310 | 1.2305 | 1.8322 | 1.108x | 0.853x | 1.001x |
| scroll_ascii | 0.9787 | 0.9969 | 1.8666 | 1.062x | 0.918x | 1.002x |
| scroll_unicode | 0.9359 | 1.0716 | 1.8280 | 1.032x | 0.956x | 1.000x |
| scroll_emoji | 1.1443 | 0.7627 | 1.8699 | 1.051x | 0.928x | 0.999x |

## Decision

Accepted.

The validation rerun reproduced the large improvement. It improves the weighted
score by about 14.1% over the previous accepted state and improves every workload
wall-time ratio, including the two highest-priority workloads:

- cursor updates: about `1.287x` wall improvement versus previous accepted,
- repaint: about `1.108x` wall improvement versus previous accepted.

CPU ratios also improve substantially across all workloads. RSS is effectively
flat. The change is not a fallback or benchmark-specific renderer switch; it is a
renderer-local elimination of redundant default-background drawing after a
full-frame clear.

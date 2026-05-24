# Accepted experiment: smaller GPU glyph atlases

## Hypothesis

The renderer allocates 2048x2048 alpha/color glyph atlases. Under llvmpipe those
textures are CPU-side allocations/uploads, and the benchmark uses a small steady
set of glyphs. Reducing atlas dimensions to 1024x1024 should lower RSS and
software-GL texture work while preserving the GPU renderer path. If an unusually
large glyph set fills the atlas, the existing atlas reset path still handles it.

## Patch summary

In `render/gpu.c`, changed the GPU atlas dimensions initialized by `gpuinit()`:

```c
gpu.atlasw = 1024;
gpu.atlash = 1024;
```

This applies to both the main alpha atlas and the lazily-allocated color emoji
atlas. The change does not disable `gpudraw`, switch to Xft, detect llvmpipe for
a fallback, or bypass the GPU renderer. Fractional scaling and normal GPU glyph
rendering behavior remain on the same path.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/smaller-atlas-1024/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/smaller-atlas-1024-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-smallatlas-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/smaller-atlas-1024-validate
```

## Validation result versus accepted lazy-color-atlas state

Previous accepted score: `0.709839`  
Experiment validation score: `0.710730`  
Relative score: `1.001x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5862 | 2.8308 | 1.8514 | 1.009x | 0.998x | 0.990x |
| repaint | 0.7048 | 1.5481 | 1.8301 | 0.998x | 1.006x | 0.991x |
| scroll_ascii | 0.8903 | 1.1351 | 1.8613 | 0.988x | 1.013x | 0.992x |
| scroll_unicode | 0.8638 | 1.1946 | 1.8274 | 1.002x | 0.994x | 0.991x |
| scroll_emoji | 1.0773 | 0.8517 | 1.8694 | 1.012x | 0.993x | 0.956x |

## Decision

Accepted.

The improvement is modest but reproduced on the validation run. It improves the
weighted score, materially improves GPU RSS ratios across all workloads, improves
cursor wall ratio, and does not materially regress repaint. The code change is
small and renderer-local, with the existing atlas reset path preserving behavior
when a workload needs more glyph capacity than the smaller atlas can hold.

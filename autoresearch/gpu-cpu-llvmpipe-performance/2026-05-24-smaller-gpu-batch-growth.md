# Accepted experiment: smaller GPU batch growth slack

## Hypothesis

`gpubatchalloc()` grows each client-side vertex batch by the requested vertices
plus a 4096-vertex slack. Under llvmpipe, these are CPU-side allocations and
cache/RSS footprint. Reducing the slack to 2048 vertices may keep batching
behavior intact while avoiding some over-allocation, especially for overlay and
smaller workloads.

## Patch summary

In `render/gpu.c`, changed the batch growth slack:

```c
b->cap = MAX(b->cap * 2, b->len + n + 2048);
```

Previously this used `+ 4096`.

This preserves the real GPU renderer path: it still batches quads and draws with
`glDrawArrays`; it does not fall back to Xft, disable `gpudraw`, or bypass the
GPU renderer.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/batch-growth-2048/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/batch-growth-2048-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-batch2048-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/batch-growth-2048-validate
```

## Validation result versus accepted smaller-atlas state

Previous accepted score: `0.710730`  
Experiment validation score: `0.712101`  
Relative score: `1.002x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5835 | 2.7886 | 1.8509 | 0.995x | 0.985x | 1.000x |
| repaint | 0.7075 | 1.5679 | 1.8295 | 1.004x | 1.013x | 1.000x |
| scroll_ascii | 0.9180 | 1.1065 | 1.8602 | 1.031x | 0.975x | 0.999x |
| scroll_unicode | 0.8572 | 1.2255 | 1.8288 | 0.992x | 1.026x | 1.001x |
| scroll_emoji | 1.0658 | 0.8626 | 1.8690 | 0.989x | 1.013x | 1.000x |

## Decision

Accepted.

The validation run improved the weighted score and did not materially regress the
high-priority workloads: cursor wall was slightly lower but cursor CPU improved,
and repaint wall improved while repaint CPU moved within normal benchmark noise.
The code change is renderer-local and preserves the same batching/drawing model
with less over-allocation slack.

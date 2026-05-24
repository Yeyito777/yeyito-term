# Failed experiment: direct triangle vertex stores

## Hypothesis

The accepted triangle batch path constructs four temporary `GpuVertex` values and
then stores six vertices (`a, b, c, a, c, d`). Writing all six vertices directly
might reduce local temporary/copy overhead in `gpubatchquad()` under llvmpipe
workloads.

## Patch summary

In `render/gpu.c`, changed `gpubatchquad()` from temporary vertices to direct
compound-literal stores into `v[0]` through `v[5]`.

This preserved the actual GPU renderer path, triangle batching, fractional
scaling, and glyph/emoji behavior. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/direct-triangle-vertices/result.json`

## Result versus accepted triangle-batch state

Accepted score: `0.712339`  
Experiment score: `0.706158`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5790 | 2.8392 | 1.8504 | 0.984x | 0.996x | 0.999x |
| repaint | 0.7010 | 1.5692 | 1.8296 | 0.993x | 1.022x | 1.000x |
| scroll_ascii | 0.8965 | 1.1335 | 1.8628 | 0.986x | 1.016x | 0.999x |
| scroll_unicode | 0.8566 | 1.2052 | 1.8280 | 1.006x | 0.970x | 1.001x |
| scroll_emoji | 1.0781 | 0.8926 | 1.8698 | 1.007x | 1.053x | 1.000x |

## Decision

Rejected and reverted.

Direct stores regressed the weighted score and the high-priority cursor/repaint
wall ratios. The accepted temporary-vertex form likely gives the compiler a
better representation and remains clearer.

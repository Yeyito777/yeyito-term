# Failed experiment: 512x512 GPU glyph atlases

## Hypothesis

After the accepted 1024x1024 atlas change, reducing the GPU glyph atlas further
to 512x512 might lower llvmpipe RSS and texture allocation/upload cost while the
existing atlas reset path preserves behavior for larger glyph sets.

## Patch summary

Changed the atlas dimensions in `render/gpu.c` from the accepted 1024x1024 to
512x512:

```c
gpu.atlasw = 512;
gpu.atlash = 512;
```

This remained on the real GPU renderer path and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/smaller-atlas-512/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/smaller-atlas-512-validate/result.json`

## Validation result versus accepted 1024x1024 atlas state

Accepted score: `0.710730`  
512x512 validation score: `0.710085`  
Relative score: `0.999x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5824 | 2.8152 | 1.8459 | 0.994x | 0.995x | 0.997x |
| repaint | 0.7001 | 1.5794 | 1.8262 | 0.993x | 1.020x | 0.998x |
| scroll_ascii | 0.9027 | 1.1317 | 1.8551 | 1.014x | 0.997x | 0.997x |
| scroll_unicode | 0.8676 | 1.2005 | 1.8232 | 1.004x | 1.005x | 0.998x |
| scroll_emoji | 1.0797 | 0.8453 | 1.8444 | 1.002x | 0.992x | 0.987x |

## Decision

Rejected and reverted.

The first run appeared promising, but it was contaminated by an Xft repaint CPU
outlier and did not reproduce. The validation run regressed the weighted score
and slightly hurt the high-priority cursor/repaint wall ratios relative to the
accepted 1024x1024 atlas. The accepted atlas size remains 1024x1024.

# Failed experiment: 2048x2048 GPU atlases

## Hypothesis

The accepted atlas size is 1024x1024. Increasing both the grayscale and color GPU
atlases to 2048x2048 might reduce atlas resets and packing pressure in repaint or
unicode/emoji workloads, potentially improving llvmpipe throughput despite the
larger texture allocations.

## Patch summary

In `render/gpu.c`, changed GPU atlas initialization from:

```c
gpu.atlasw = 1024;
gpu.atlash = 1024;
```

to:

```c
gpu.atlasw = 2048;
gpu.atlash = 2048;
```

No renderer path, batching, GL state, glyph/emoji semantics, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The color atlas remained lazily allocated. The benchmark still used
the actual GPU renderer under llvmpipe and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/atlas-2048/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.852955`  
Relative score: `0.980x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8025 | 1.6941 | 1.8678 | 0.956x | 1.034x | 1.009x |
| repaint | 0.8659 | 1.1844 | 1.8492 | 1.023x | 0.987x | 1.010x |
| scroll_ascii | 0.9419 | 1.0361 | 1.8811 | 0.956x | 1.051x | 1.008x |
| scroll_unicode | 0.9276 | 1.0760 | 1.8431 | 0.975x | 1.031x | 1.007x |
| scroll_emoji | 1.1114 | 0.7876 | 1.9536 | 0.980x | 1.021x | 1.044x |

## Decision

Rejected and reverted.

The larger atlases improved repaint wall, but cursor, ASCII, unicode, emoji, CPU,
and RSS regressed enough to lower the weighted score substantially. The accepted
1024x1024 atlas size remains the better llvmpipe tradeoff.

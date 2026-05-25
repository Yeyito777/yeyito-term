# Failed experiment: 768x768 GPU glyph atlases

## Hypothesis

The accepted 1024x1024 glyph/color atlases improved llvmpipe behavior versus the
original 2048x2048 atlases, while 512x512 atlases were too small. A middle-ground
768x768 atlas might reduce texture memory/RSS further without causing enough atlas
resets to hurt wall/CPU time.

## Patch summary

In `render/gpu.c`, changed atlas dimensions in `gpuinit()` from:

```c
gpu.atlasw = 1024;
gpu.atlash = 1024;
```

to:

```c
gpu.atlasw = 768;
gpu.atlash = 768;
```

The existing atlas reset path, glyph placement, lazy color atlas storage,
fractional scaling, GPU text/emoji behavior, triangle batches, alpha test, solid
no-blend behavior, accepted clear-color cache, and cleared-background skip were
otherwise unchanged. This did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/atlas-768/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/atlas-768-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.857681`  
Relative score: `0.991x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8221 | 1.7142 | 1.8455 | 0.989x | 1.023x | 0.997x |
| repaint | 0.8304 | 1.2310 | 1.8307 | 0.991x | 1.011x | 1.000x |
| scroll_ascii | 0.9856 | 0.9899 | 1.8617 | 0.994x | 0.995x | 0.998x |
| scroll_unicode | 0.9391 | 1.0631 | 1.8279 | 0.988x | 1.000x | 1.000x |
| scroll_emoji | 1.1381 | 0.7698 | 1.8548 | 0.994x | 0.995x | 0.993x |

## Decision

Rejected and reverted.

The initial run was promising (`0.870664`), but validation failed to reproduce it.
The validated score regressed, and cursor/repaint wall ratios were both below the
accepted 1024x1024 atlas state. The accepted 1024x1024 atlas remains the better
size under this llvmpipe benchmark.

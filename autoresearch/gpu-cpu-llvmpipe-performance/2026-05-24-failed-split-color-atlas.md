# Failed experiment: separate 512x512 color atlas

## Hypothesis

Keep the accepted 1024x1024 main alpha atlas, but use a smaller 512x512 color
emoji atlas with separate color-atlas packing state. This should reduce emoji
texture/RSS cost without affecting the main text atlas capacity.

## Patch summary

The experiment changed `render/gpu.c` to add separate color atlas dimensions and
pen state:

- `catlasw` / `catlash`
- `cpenx` / `cpeny` / `crowh`
- color glyphs were packed in the color atlas and normalized against color atlas
  dimensions in `gpubatchglyph()`.
- normal glyphs kept the accepted 1024x1024 main atlas.

This remained on the real GPU renderer path and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/split-color-atlas-512/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/split-color-atlas-512-validate/result.json`

## Validation result versus accepted 1024x1024 atlas state

Accepted score: `0.710730`  
Experiment validation score: `0.707990`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5779 | 2.8567 | 1.8517 | 0.986x | 1.009x | 1.000x |
| repaint | 0.7013 | 1.5518 | 1.8320 | 0.995x | 1.002x | 1.001x |
| scroll_ascii | 0.9029 | 1.1210 | 1.8603 | 1.014x | 0.988x | 0.999x |
| scroll_unicode | 0.8658 | 1.2137 | 1.8269 | 1.002x | 1.016x | 1.000x |
| scroll_emoji | 1.0685 | 0.8687 | 1.8524 | 0.992x | 1.020x | 0.991x |

## Decision

Rejected and reverted.

The first run looked positive, but the validation run did not reproduce. It
regressed weighted score and hurt high-priority cursor/repaint wall ratios. The
extra branch/state in the glyph batching/packing path also did not buy enough RSS
improvement to justify the complexity. Keep the simpler accepted 1024x1024 atlas
for both normal and color glyphs.

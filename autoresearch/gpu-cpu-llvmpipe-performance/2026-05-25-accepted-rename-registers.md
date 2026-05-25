# Accepted experiment: add -frename-registers

## Hypothesis

The current llvmpipe GPU path is a single large translation unit with tight
per-cell, batching, GL client-array, and terminal update loops. GCC's post-register
allocation renaming can sometimes reduce false dependencies and improve scheduling
in such branch-heavy hot paths. `-frename-registers` might improve the actual GPU
renderer under Mesa llvmpipe without changing renderer behavior.

## Patch summary

In `config.mk`, the accepted change adds `-frename-registers`:

```make
CFLAGS = -O3 -march=native -flto -frename-registers
```

No renderer source behavior changed. The benchmark still compared same-source Xft
`gpudraw=0` versus the actual GPU renderer `gpudraw=1` under Mesa llvmpipe. This
keeps the real GPU path, text rendering, color emoji, fractional scaling, triangle
batches, solid no-blend path, textured alpha-test path, clear-color cache,
cleared-background skip, and accepted vimnav current-line guard. It does not use
Xft fallback, disable/bypass `gpudraw`, switch renderers, or detect llvmpipe.

## Validation

Before benchmarking:

- `make`
- `make test_gpu_regressions`

Both passed.

Because the initial benchmark beat the accepted frontier, it was validated with a
longer 9-iteration / 2-warmup run.

## Benchmark results

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/rename-registers/result.json`
- Score: `0.8803779516160227`
- Relative to previous accepted `92d5749`: `1.0116x`

Validation result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/rename-registers-validate/result.json`
- Score: `0.8761148402249465`
- Relative to previous accepted `92d5749`: `1.0067x`

Previous accepted frontier:

- Commit: `92d5749 Guard GPU vimnav current line lookup`
- Score: `0.8702497462749965`
- Result: `autoresearch/gpu-cpu-llvmpipe-performance/runs/vimnav-curline-guard-validate/result.json`

## Validation result versus previous accepted frontier

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8322 | 1.6604 | 1.8502 | 0.992x | 1.014x | 0.999x |
| repaint | 0.8597 | 1.1716 | 1.8312 | 1.015x | 0.976x | 1.001x |
| scroll_ascii | 1.0088 | 0.9575 | 1.8634 | 1.024x | 0.971x | 0.999x |
| scroll_unicode | 0.9638 | 1.0470 | 1.8299 | 1.013x | 1.003x | 1.000x |
| scroll_emoji | 1.1295 | 0.7719 | 1.8702 | 0.996x | 1.000x | 0.999x |

## Decision

Accepted.

The validation run remained above the previous accepted frontier with a clear
weighted gain (`0.876115` vs `0.870250`, about `+0.67%`). Repaint, ASCII, and
unicode wall ratios improved on validation, while cursor and emoji were close and
the weighted score stayed positive. This becomes the new accepted frontier.

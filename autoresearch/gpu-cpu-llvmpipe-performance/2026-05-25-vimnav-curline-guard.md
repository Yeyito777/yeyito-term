# Accepted experiment: guard vimnav current-line lookup in GPU row renderer

## Hypothesis

`gpudrawline()` calls `vimnav_curline_y()` once per rendered row so it can avoid
the default-background fast path on the highlighted vimnav current line. In the
normal benchmark state, vimnav mode is inactive. Calling the full helper still
walks through the vimnav current-line policy checks for every row even though the
answer is always `-1`. Guarding the helper call with the exported `vimnav.mode`
field should remove that per-row work while preserving behavior whenever vimnav is
active.

## Patch summary

In `render/gpu.c`, changed:

```c
int vimline = vimnav_curline_y();
```

to:

```c
int vimline = vimnav.mode ? vimnav_curline_y() : -1;
```

When vimnav is inactive this avoids the helper call. When vimnav is active, the
same helper still decides whether a row should be highlighted, preserving normal,
visual, zsh-visual, prompt-space, and below-prompt behavior.

This preserves the actual GPU renderer path, fractional scaling, glyph/emoji
rendering, triangle batches, alpha test, solid no-blend behavior, accepted
clear-color cache, and cleared-background skip. It does not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/vimnav-curline-guard/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/vimnav-curline-guard-validate/result.json`

## Validation result versus previous accepted clear-color state cache

Previous accepted score: `0.865042`  
Experiment validation score: `0.870250`  
Relative score: `1.006x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs previous | CPU vs previous | RSS vs previous |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8393 | 1.6381 | 1.8518 | 1.009x | 0.978x | 1.001x |
| repaint | 0.8467 | 1.2002 | 1.8301 | 1.010x | 0.985x | 1.000x |
| scroll_ascii | 0.9855 | 0.9860 | 1.8652 | 0.993x | 0.991x | 1.000x |
| scroll_unicode | 0.9515 | 1.0440 | 1.8296 | 1.001x | 0.982x | 1.001x |
| scroll_emoji | 1.1339 | 0.7716 | 1.8713 | 0.990x | 0.997x | 1.002x |

## Decision

Accepted.

The improvement validated above the previous frontier. It improves the weighted
score by about `0.6%`, with clear gains on the high-weight cursor and repaint wall
ratios. ASCII and emoji wall ratios are slightly lower than the previous frontier,
but the weighted score and primary workloads improve, and the change is small and
behavior-preserving for active vimnav sessions.

New accepted score: `0.870250`.

# Failed experiment: fast path normal GPU font face

## Hypothesis

Most benchmark glyphs are rendered with the normal font face, but `gpufaceidx()`
still evaluates italic/bold conditions and returns through the generic fallback
logic for every glyph lookup. Early-returning `FRC_NORMAL` when neither bold nor
italic is set could reduce per-glyph CPU work during glyph cache lookup and upload
without changing rendered output.

## Patch summary

In `render/gpu.c`, the experiment added to `gpufaceidx()`:

```c
if (!(mode & (ATTR_ITALIC|ATTR_BOLD)))
    return FRC_NORMAL;
```

before the existing style-face selection. GPU initialization already requires the
normal face, so this preserves behavior for normal glyphs. Bold/italic/bold-italic
paths were otherwise unchanged.

The actual GPU renderer path, glyph/emoji behavior, fractional scaling, triangle
batches, alpha test, solid no-blend behavior, accepted clear-color cache, and
cleared-background skip were preserved. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/normal-face-fastpath/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/normal-face-fastpath-validate/result.json`

## Validation result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment validation score: `0.860947`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8356 | 1.6441 | 1.8482 | 1.005x | 0.981x | 0.999x |
| repaint | 0.8316 | 1.2197 | 1.8322 | 0.992x | 1.001x | 1.001x |
| scroll_ascii | 0.9785 | 0.9854 | 1.8667 | 0.986x | 0.991x | 1.001x |
| scroll_unicode | 0.9286 | 1.0838 | 1.8299 | 0.977x | 1.020x | 1.001x |
| scroll_emoji | 1.1223 | 0.7723 | 1.8695 | 0.980x | 0.998x | 1.001x |

## Decision

Rejected and reverted.

The initial run looked positive (`0.869481`) with improved repaint wall, but
validation failed to reproduce it. The validated total score fell below accepted,
and repaint/scrolling wall ratios regressed despite a small cursor-wall gain.

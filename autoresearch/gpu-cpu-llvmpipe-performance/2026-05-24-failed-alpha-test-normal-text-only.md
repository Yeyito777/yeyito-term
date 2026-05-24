# Failed experiment: alpha-test normal text only

## Hypothesis

After accepting alpha testing for all textured batches, restrict alpha testing to
normal monochrome glyph batches (`textured == 1`) and disable it for color emoji
batches (`textured == 2`). Color emoji already uses a different blend function;
avoiding alpha-test state for that path might reduce llvmpipe overhead while
keeping the normal glyph benefit.

## Patch summary

In `render/gpu.c`, changed `gpudrawbatch()` from the accepted all-textured alpha
state to:

```c
if (textured == 1) {
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.0f);
} else {
    glDisable(GL_ALPHA_TEST);
}
```

inside the textured branch. Solid batches still disabled blending, alpha testing,
and texturing as in the accepted state.

This preserved the actual GPU renderer path and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/alpha-test-normal-text-only/result.json`

## Result versus accepted all-textured alpha-test state

Accepted score: `0.756262`  
Experiment score: `0.750871`  
Relative score: `0.993x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6477 | 2.4739 | 1.8510 | 0.992x | 1.024x | 1.000x |
| repaint | 0.7385 | 1.4618 | 1.8302 | 0.985x | 1.013x | 1.000x |
| scroll_ascii | 0.9289 | 1.0826 | 1.8644 | 1.008x | 0.997x | 1.000x |
| scroll_unicode | 0.8853 | 1.1690 | 1.8263 | 0.976x | 1.042x | 0.999x |
| scroll_emoji | 1.1064 | 0.8233 | 1.8703 | 1.016x | 1.001x | 1.000x |

## Decision

Rejected and reverted.

Restricting alpha testing helped emoji and ASCII slightly, but it regressed the
weighted score and the two highest-weight workloads, cursor updates and repaint.
The accepted all-textured alpha-test state remains better under this benchmark.

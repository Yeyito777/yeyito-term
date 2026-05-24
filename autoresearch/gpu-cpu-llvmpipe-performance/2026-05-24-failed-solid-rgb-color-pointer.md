# Failed experiment: RGB color pointer for solid batches only

## Hypothesis

After accepting disabled blending for solid batches, solid rectangles do not need
vertex alpha. Keeping the existing 4-float vertex layout but using a
three-component color pointer only for solid batches might let llvmpipe skip one
color component without changing the textured glyph/emoji path or altering the
batch memory layout.

## Patch summary

In `render/gpu.c`, changed color-array setup in `gpudrawbatch()` from:

```c
glColorPointer(4, GL_FLOAT, sizeof(GpuVertex), coff);
```

to:

```c
glColorPointer(textured ? 4 : 3, GL_FLOAT, sizeof(GpuVertex), coff);
```

Textured batches kept RGBA colors; solid batches used RGB colors with the same
stride. The renderer stayed on the actual GPU path with triangle batches,
fractional scaling, glyph atlases, color emoji, and no-blend solid batches. It
did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-batch-rgb-color-pointer/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/solid-batch-rgb-color-pointer-validate/result.json`

## Validation result versus accepted disable-solid-blend state

Accepted score: `0.750395`  
Validation score: `0.746841`  
Relative score: `0.995x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6580 | 2.4483 | 1.8517 | 1.020x | 0.994x | 1.001x |
| repaint | 0.7214 | 1.4953 | 1.8313 | 0.969x | 1.041x | 1.001x |
| scroll_ascii | 0.9322 | 1.0863 | 1.8622 | 1.010x | 1.005x | 0.998x |
| scroll_unicode | 0.8707 | 1.1774 | 1.8271 | 0.984x | 1.009x | 1.000x |
| scroll_emoji | 1.0893 | 0.8239 | 1.8704 | 1.004x | 0.996x | 0.999x |

## Decision

Rejected and reverted.

The initial run looked positive, but the validation run did not reproduce. It
regressed the weighted score and materially hurt repaint wall/CPU. Keeping the
same four-component color pointer for all batches remains better in the accepted
llvmpipe renderer state.

# Accepted experiment: alpha-test textured GPU batches

## Hypothesis

The GPU renderer draws monochrome glyphs and color emoji through textured quads.
Many glyph atlas pixels are fully transparent. Under llvmpipe, enabling the fixed
function alpha test for textured batches may let the software rasterizer discard
fully transparent fragments before blend work, while solid background/deco
batches continue using the accepted no-blend path.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` now enables alpha testing only while drawing
textured batches:

```c
if (textured) {
    glEnable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.0f);
    glEnable(GL_TEXTURE_2D);
    ...
} else {
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
    ...
}
```

This keeps the actual GPU renderer path intact. It does not fallback to Xft,
disable `gpudraw`, or detect llvmpipe to switch renderers. Textured glyphs and
color emoji still use their existing texture atlas and blend-function paths;
only fully transparent texels are rejected by fixed-function alpha testing.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/text-alpha-test/result.json`

Validation rerun 1 with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/text-alpha-test-validate/result.json`

Validation rerun 2 with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/text-alpha-test-validate2/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-alphatest-val2 \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/text-alpha-test-validate2
```

## Validation result versus accepted disable-solid-blend state

Previous accepted score: `0.750395`  
Initial score: `0.750628`  
Validation 1 score: `0.760214`  
Validation 2 score: `0.756262`

Validation 2 relative score: `1.008x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6526 | 2.4167 | 1.8517 | 1.011x | 0.981x | 1.001x |
| repaint | 0.7498 | 1.4432 | 1.8305 | 1.007x | 1.005x | 1.001x |
| scroll_ascii | 0.9214 | 1.0862 | 1.8637 | 0.998x | 1.005x | 0.999x |
| scroll_unicode | 0.9067 | 1.1214 | 1.8283 | 1.025x | 0.961x | 1.001x |
| scroll_emoji | 1.0889 | 0.8221 | 1.8712 | 1.004x | 0.994x | 1.000x |

## Decision

Accepted.

The first run was only barely positive, but both validation reruns stayed above
the previous accepted score. The second validation also improved the two highest
weighted wall-time workloads, cursor updates and repaint, while keeping RSS
essentially flat. This is a small fixed-function state change that preserves the
renderer semantics and complements the accepted no-blending path for solid
batches.

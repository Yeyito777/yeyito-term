# Accepted experiment: draw GPU batches as triangles

## Hypothesis

The renderer batches every cell/rect/glyph quad as `GL_QUADS`. llvmpipe has to
handle that legacy primitive mode in software. Expanding each quad to two
triangles client-side and drawing with `GL_TRIANGLES` may reduce llvmpipe's
primitive conversion/compatibility overhead, even though it increases vertex
count from 4 to 6 per quad.

## Patch summary

In `render/gpu.c`:

- `gpubatchquad()` now appends six vertices per quad: two triangles
  `(a, b, c)` and `(a, c, d)`.
- `gpudrawbatch()` now calls:

```c
glDrawArrays(GL_TRIANGLES, 0, b->len);
```

instead of `GL_QUADS`.

The renderer still uses the same GPU codepath, texture atlases, fractional
scaling math, batching model, and glyph/emoji rendering behavior. This does not
fallback to Xft, disable `gpudraw`, or bypass the GPU renderer.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/triangle-batches/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/triangle-batches-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-triangles-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/triangle-batches-validate
```

## Validation result versus accepted 2048-slack state

Previous accepted score: `0.712101`  
Experiment validation score: `0.712339`  
Relative score: `1.000x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5887 | 2.8519 | 1.8514 | 1.009x | 1.023x | 1.000x |
| repaint | 0.7061 | 1.5352 | 1.8298 | 0.998x | 0.979x | 1.000x |
| scroll_ascii | 0.9091 | 1.1153 | 1.8644 | 0.990x | 1.008x | 1.002x |
| scroll_unicode | 0.8511 | 1.2431 | 1.8257 | 0.993x | 1.014x | 0.998x |
| scroll_emoji | 1.0707 | 0.8478 | 1.8691 | 1.005x | 0.983x | 1.000x |

## Decision

Accepted.

The validated score improvement is small, but it is positive and the high-priority
cursor/repaint workloads are not materially regressed: cursor wall improves, and
repaint wall is essentially unchanged while repaint CPU improves. The change is
also tasteful renderer modernization: it removes reliance on legacy `GL_QUADS`
and lets llvmpipe consume triangles directly.

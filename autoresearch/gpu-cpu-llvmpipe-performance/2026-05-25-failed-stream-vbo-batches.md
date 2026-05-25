# Failed experiment: stream batches through a VBO

## Hypothesis

The accepted renderer uses OpenGL client arrays. Under llvmpipe, each draw may
copy or walk client memory repeatedly. Streaming each batch into a single
`GL_ARRAY_BUFFER` with `glBufferData(..., GL_STREAM_DRAW)` before drawing could let
Mesa handle the vertex stream more efficiently than client arrays, especially for
large text/repaint batches.

## Patch summary

In `render/gpu.c`, the experiment loaded buffer-object entry points with
`glXGetProcAddressARB`, created one `gpu.vbo`, and changed `gpudrawbatch()` to use
it when available:

```c
gpu.BindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
gpu.BufferData(GL_ARRAY_BUFFER, b->len * sizeof(GpuVertex), b->v, GL_STREAM_DRAW);
voff = (const void *)0;
toff = (const void *)(2 * sizeof(GLfloat));
coff = (const void *)(4 * sizeof(GLfloat));
```

After each draw it unbound `GL_ARRAY_BUFFER`. If VBO entry points were unavailable,
the existing client-array path remained as a fallback for portability. Rendering
semantics, GL state, batching order, actual GPU path, fractional scaling,
glyph/emoji behavior, accepted clear-color cache, cleared-background skip, and
accepted vimnav row guard were otherwise unchanged. It did not fallback to Xft or
bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/stream-vbo-batches/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.851359`  
Relative score: `0.978x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8270 | 1.6873 | 1.8508 | 0.985x | 1.030x | 0.999x |
| repaint | 0.8042 | 1.2882 | 1.8315 | 0.950x | 1.073x | 1.001x |
| scroll_ascii | 0.9863 | 0.9748 | 1.8671 | 1.001x | 0.989x | 1.001x |
| scroll_unicode | 0.9430 | 1.0681 | 1.8308 | 0.991x | 1.023x | 1.001x |
| scroll_emoji | 1.1272 | 0.7725 | 1.8693 | 0.994x | 1.001x | 0.999x |

## Decision

Rejected and reverted.

Streaming through a VBO did not help llvmpipe here. The extra buffer-object upload
and bind/unbind overhead hurt cursor and repaint substantially, and the total score
fell well below accepted. The accepted client-array path remains faster for this
renderer state.

# Failed experiment: GL_QUADS only for textured glyph batches

## Hypothesis

The accepted triangle batching expands every quad to six vertices. For glyph and
color-emoji batches, this means each visible glyph sends six interleaved vertices
through llvmpipe. Drawing only textured batches with `GL_QUADS` could reduce
client-array bandwidth for text-heavy workloads while keeping solid background and
decoration batches on accepted `GL_TRIANGLES`.

This differs from earlier all-quad experiments by applying quads only to glyph
texture batches (`gpu.text`, `gpu.ctext`, and overlay equivalents), where vertex
count dominates repaint/scrolling.

## Patch summary

In `render/gpu.c`, `gpubatchglyph()` allocated four vertices in quad order instead
of calling the accepted six-vertex triangle helper. `gpudrawbatch()` drew textured
batches with:

```c
glDrawArrays(textured ? GL_QUADS : GL_TRIANGLES, 0, b->len);
```

Solid/background/deco batches retained the accepted triangle path. GL state,
blending, alpha test, actual GPU renderer path, fractional scaling, glyph/emoji
behavior, accepted clear-color cache, cleared-background skip, and accepted vimnav
row guard were otherwise unchanged. It did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/textured-quads/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.861127`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8408 | 1.6488 | 1.8563 | 1.002x | 1.007x | 1.002x |
| repaint | 0.8290 | 1.2342 | 1.8300 | 0.979x | 1.028x | 1.000x |
| scroll_ascii | 0.9724 | 1.0008 | 1.8613 | 0.987x | 1.015x | 0.998x |
| scroll_unicode | 0.9274 | 1.0757 | 1.8266 | 0.975x | 1.030x | 0.998x |
| scroll_emoji | 1.1381 | 0.7638 | 1.8671 | 1.004x | 0.990x | 0.998x |

## Decision

Rejected and reverted.

Textured quads slightly improved cursor and emoji wall ratios, but repaint,
ASCII, and unicode regressed. The llvmpipe cost of `GL_QUADS` conversion/state did
not justify the smaller textured vertex streams. Keep the accepted all-triangle
batch format.

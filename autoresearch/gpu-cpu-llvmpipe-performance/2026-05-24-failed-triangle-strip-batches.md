# Failed experiment: draw batches as triangle strips

## Hypothesis

The accepted renderer expands every quad to two independent triangles, using six
vertices per quad. Drawing each batch as a single `GL_TRIANGLE_STRIP` with
degenerate joins between quads could reduce vertex count to four vertices for the
first quad and five for each subsequent quad while staying on a triangle-based
software-GL path.

## Patch summary

In `render/gpu.c`, changed `gpubatchquad()` to emit triangle-strip ordered
vertices:

- first quad: `a, b, d, c`,
- later quads: `a, a, b, d, c` so the repeated `a` vertices create degenerate
  bridge triangles from the previous strip.

`gpudrawbatch()` drew the batch with:

```c
glDrawArrays(GL_TRIANGLE_STRIP, 0, b->len);
```

This preserved the actual GPU renderer path, batching model, texture atlases,
fractional scaling, glyph/emoji behavior, the accepted clear-color cache,
cleared-background skip, alpha test, and solid no-blend behavior. It did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/triangle-strip-batches/result.json`

## Result versus accepted clear-color state cache

Accepted score: `0.865042`  
Experiment score: `0.827921`  
Relative score: `0.957x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8224 | 1.7402 | 1.8496 | 0.989x | 1.039x | 1.000x |
| repaint | 0.7847 | 1.3307 | 1.8337 | 0.936x | 1.092x | 1.002x |
| scroll_ascii | 0.9383 | 1.0393 | 1.8708 | 0.946x | 1.045x | 1.003x |
| scroll_unicode | 0.9123 | 1.1097 | 1.8305 | 0.960x | 1.044x | 1.002x |
| scroll_emoji | 1.0904 | 0.8169 | 1.8696 | 0.952x | 1.056x | 1.001x |

## Decision

Rejected and reverted.

The reduced vertex count was not worth the `GL_TRIANGLE_STRIP`/degenerate-join
path under llvmpipe. Wall ratios regressed across every workload, especially
repaint. The accepted plain `GL_TRIANGLES` batch remains substantially better.

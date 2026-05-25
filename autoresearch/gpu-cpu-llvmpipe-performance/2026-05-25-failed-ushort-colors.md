# Failed experiment: unsigned-short vertex colors

## Hypothesis

`GpuVertex` stores four vertex color components as floats. The renderer's source
colors ultimately come from 8-bit truecolor values or 16-bit Xft palette colors,
and OpenGL client color arrays normalize integer types. Storing colors as
`GLushort` could shrink each vertex from 32 bytes to 24 bytes while preserving
palette precision, reducing llvmpipe client-array bandwidth and cache pressure.

## Patch summary

In `render/gpu.c`, the experiment changed `GpuVertex` color fields from:

```c
GLfloat r, g, b, a;
```

to:

```c
GLushort r, g, b, a;
```

It added a helper to convert float colors to 16-bit normalized values and changed
`gpubatchquad()` to store those integer colors with alpha `65535`. The color array
setup changed from:

```c
glColorPointer(4, GL_FLOAT, sizeof(GpuVertex), coff);
```

to:

```c
glColorPointer(4, GL_UNSIGNED_SHORT, sizeof(GpuVertex), coff);
```

The actual GPU renderer path, glyph/emoji rendering, fractional scaling, triangle
batches, alpha test, solid no-blend behavior, accepted clear-color cache,
cleared-background skip, and accepted vimnav row guard were preserved. It did not
fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/ushort-colors/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.857608`  
Relative score: `0.985x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8336 | 1.6782 | 1.8513 | 0.993x | 1.024x | 1.000x |
| repaint | 0.8246 | 1.2467 | 1.8299 | 0.974x | 1.039x | 1.000x |
| scroll_ascii | 0.9741 | 0.9978 | 1.8617 | 0.988x | 1.012x | 0.998x |
| scroll_unicode | 0.9349 | 1.0722 | 1.8255 | 0.982x | 1.027x | 0.998x |
| scroll_emoji | 1.1394 | 0.7704 | 1.8701 | 1.005x | 0.998x | 0.999x |

## Decision

Rejected and reverted.

The smaller vertex format helped emoji wall slightly and RSS a tiny amount, but it
regressed cursor, repaint, ASCII, and unicode wall ratios. The conversion overhead
and/or llvmpipe's handling of unsigned-short color arrays outweighs any bandwidth
benefit. Keep float vertex colors.

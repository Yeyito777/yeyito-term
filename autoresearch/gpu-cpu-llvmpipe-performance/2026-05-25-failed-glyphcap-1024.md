# Failed experiment: grow GPU glyph vector initially to 1024 entries

## Hypothesis

`gpuglyph()` grows the glyph metadata vector in 256-entry increments. Unicode and
emoji benchmark processes can load enough glyphs to require multiple reallocations.
Starting the vector at 1024 entries on first growth could reduce realloc/copy work
for glyph-heavy workloads, at the cost of a small metadata allocation.

## Patch summary

In `render/gpu.c`, the experiment changed glyph-vector growth from:

```c
gpu.glyphcap += 256;
```

to:

```c
gpu.glyphcap = gpu.glyphcap ? gpu.glyphcap + 256 : 1024;
```

No glyph rendering, atlas placement, batching, GL state, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. It did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/glyphcap-1024/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.861882`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8422 | 1.6149 | 1.8470 | 1.003x | 0.986x | 0.997x |
| repaint | 0.8294 | 1.2506 | 1.8285 | 0.980x | 1.042x | 0.999x |
| scroll_ascii | 0.9793 | 1.0047 | 1.8661 | 0.994x | 1.019x | 1.000x |
| scroll_unicode | 0.9290 | 1.0842 | 1.8286 | 0.976x | 1.038x | 0.999x |
| scroll_emoji | 1.1326 | 0.7668 | 1.8678 | 0.999x | 0.994x | 0.998x |

## Decision

Rejected and reverted.

The larger initial glyph vector helped cursor wall slightly and reduced measured
RSS ratio a little, but repaint/unicode/ASCII wall ratios regressed and the
weighted score fell below the accepted vimnav-guard state. Keep the 256-entry
growth policy.

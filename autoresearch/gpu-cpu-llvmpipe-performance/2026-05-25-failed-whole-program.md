# Failed experiment: build with -fwhole-program

## Hypothesis

The project links a single `st` executable with LTO, and the GPU renderer is
included into `x.c`. Adding `-fwhole-program` might let GCC assume non-exported
symbols are not externally interposed and optimize calls/globals more aggressively,
reducing CPU overhead in the llvmpipe renderer and same-source Xft comparison.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -fwhole-program
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/whole-program/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.868531`  
Relative score: `0.998x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8398 | 1.6410 | 1.8474 | 1.001x | 1.002x | 0.998x |
| repaint | 0.8363 | 1.2137 | 1.8293 | 0.988x | 1.011x | 1.000x |
| scroll_ascii | 0.9986 | 0.9731 | 1.8690 | 1.013x | 0.987x | 1.002x |
| scroll_unicode | 0.9467 | 1.0468 | 1.8286 | 0.995x | 1.003x | 0.999x |
| scroll_emoji | 1.1282 | 0.7711 | 1.8688 | 0.995x | 0.999x | 0.999x |

## Decision

Rejected and reverted.

The flag improved cursor and ASCII wall ratios slightly, but repaint, unicode, and
emoji wall ratios remained below accepted and the weighted score did not beat the
frontier. Keep the accepted build flags.

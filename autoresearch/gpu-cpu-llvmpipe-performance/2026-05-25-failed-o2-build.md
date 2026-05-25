# Failed experiment: build with -O2 instead of -O3

## Hypothesis

The accepted build uses `-O3 -march=native -flto`. For this small renderer and a
software-GL workload, `-O3` could increase code size or choose transformations that
hurt instruction-cache behavior. Building with `-O2` while keeping `-march=native`
and LTO might improve the weighted same-source Xft-vs-llvmpipe score.

## Patch summary

In `config.mk`, changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O2 -march=native -flto
```

No renderer source, GL state, batching, glyph/emoji rendering, fractional scaling,
accepted clear-color cache, cleared-background skip, or accepted vimnav row guard
was changed. The benchmark still compared the same-source Xft and actual GPU paths
under llvmpipe; it did not fallback to Xft or bypass `gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/o2-build/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.867878`  
Relative score: `0.997x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8282 | 1.6475 | 1.8526 | 0.987x | 1.006x | 1.000x |
| repaint | 0.8530 | 1.1846 | 1.8331 | 1.008x | 0.987x | 1.002x |
| scroll_ascii | 0.9833 | 0.9855 | 1.8683 | 0.998x | 1.000x | 1.002x |
| scroll_unicode | 0.9404 | 1.0856 | 1.8316 | 0.988x | 1.040x | 1.001x |
| scroll_emoji | 1.1394 | 0.7704 | 1.8703 | 1.005x | 0.998x | 0.999x |

## Decision

Rejected and reverted.

`-O2` improved repaint and emoji wall ratios slightly, but cursor and unicode
regressed and the total score stayed below accepted. Keep the accepted `-O3` build.

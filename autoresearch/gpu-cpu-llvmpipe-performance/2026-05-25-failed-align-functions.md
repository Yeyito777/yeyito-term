# Failed experiment: build with -falign-functions=32

## Hypothesis

The accepted binary is CPU-bound under llvmpipe, and hot renderer functions such
as `gpudrawline()`, `gpuglyph()`, and `gpudrawbatch()` may benefit from stronger
function-entry alignment. Adding `-falign-functions=32` could reduce I-cache/front
end penalties without changing behavior.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto
```

to:

```make
CFLAGS = -O3 -march=native -flto -falign-functions=32
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

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/align-functions/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.861242`  
Relative score: `0.990x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8290 | 1.6444 | 1.8508 | 0.988x | 1.004x | 0.999x |
| repaint | 0.8396 | 1.2162 | 1.8309 | 0.992x | 1.013x | 1.000x |
| scroll_ascii | 0.9744 | 1.0116 | 1.8642 | 0.989x | 1.026x | 0.999x |
| scroll_unicode | 0.9453 | 1.0628 | 1.8286 | 0.993x | 1.018x | 0.999x |
| scroll_emoji | 1.1259 | 0.7824 | 1.8683 | 0.993x | 1.014x | 0.998x |

## Decision

Rejected and reverted.

The stronger alignment regressed every workload's wall ratio versus accepted and
lowered the weighted score. Keep the accepted build flags.

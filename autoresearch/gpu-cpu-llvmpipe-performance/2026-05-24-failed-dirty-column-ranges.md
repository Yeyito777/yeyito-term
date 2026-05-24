# Failed experiment: dirty column ranges

## Hypothesis

The benchmark's cursor workload changes one cell at a time, while st's dirty
tracking redraws whole dirty rows. Tracking a dirty column interval per row and
calling `xdrawline()` only for that interval might reduce GPU llvmpipe row redraw
work without bypassing the GPU renderer.

## Patch summary

The experiment added per-row dirty column ranges in `st.c`:

- `Term` gained `dirtyx1`/`dirtyx2` arrays.
- `tsetdirt()` marked full-row ranges.
- single-cell writes and `tclearregion()` widened/merged only the affected
  column ranges.
- `drawregion()` clipped `xdrawline()` to each row's dirty range.

This was a real redraw-work reduction; it did not disable `gpudraw`, switch to
Xft, or otherwise bypass the GPU renderer.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 7 --warmups 2 \
  --name st-llvmpipe-dirtyranges \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/dirty-column-ranges
```

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/dirty-column-ranges/result.json`

## Result versus accepted lazy-color-atlas state

Accepted score: `0.709839`  
Experiment score: `0.701081`  
Relative score: `0.988x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5806 | 2.8828 | 1.8685 | 0.999x | 1.016x |
| repaint | 0.7769 | 1.3782 | 1.8471 | 1.100x | 0.895x |
| scroll_ascii | 0.8041 | 1.2899 | 1.8775 | 0.893x | 1.151x |
| scroll_unicode | 0.7432 | 1.4455 | 1.8452 | 0.862x | 1.203x |
| scroll_emoji | 1.0842 | 0.9839 | 1.9405 | 1.018x | 1.148x |

## Decision

Rejected and reverted.

Although repaint wall ratio improved substantially, the weighted score regressed
and scrolling ratios became materially worse because the same-source Xft
reference benefited more from the dirty-range change than the GPU llvmpipe path.
The cursor workload also did not materially improve. This is not a good GPU CPU
renderer optimization under the benchmark's acceptance rule.

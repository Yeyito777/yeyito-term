# Failed experiment: indexed triangle GPU batches

## Hypothesis

After accepting client-side expansion from quads to triangles, use
`glDrawElements()` with four unique vertices plus six indices per quad. This
could keep llvmpipe on `GL_TRIANGLES` while avoiding the 50% vertex duplication
from plain triangle batches.

## Patch summary

The experiment changed `GpuBatch` in `render/gpu.c` to carry an index buffer:

- added `GLuint *idx` plus index length/capacity fields,
- reset index length with vertex length,
- emitted four vertices and six indices per quad,
- drew with `glDrawElements(GL_TRIANGLES, ..., GL_UNSIGNED_INT, b->idx)`,
- freed the index buffers in `gpudestroy()`.

This preserved the actual GPU renderer path and did not fallback to Xft or bypass
`gpudraw`.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/indexed-triangle-batches/result.json`

## Result versus accepted plain-triangle state

Accepted score: `0.712339`  
Experiment score: `0.709301`  
Relative score: `0.996x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.5805 | 2.8557 | 1.8492 | 0.986x | 1.001x | 0.999x |
| repaint | 0.7105 | 1.5685 | 1.8275 | 1.006x | 1.022x | 0.999x |
| scroll_ascii | 0.9031 | 1.1217 | 1.8607 | 0.993x | 1.006x | 0.998x |
| scroll_unicode | 0.8505 | 1.2191 | 1.8256 | 0.999x | 0.981x | 1.000x |
| scroll_emoji | 1.0670 | 0.8531 | 1.8679 | 0.997x | 1.006x | 0.999x |

## Decision

Rejected and reverted.

The extra index-buffer bookkeeping and `glDrawElements()` path regressed the
weighted score and hurt the high-priority cursor workload. The simpler accepted
plain-triangle batch path remains better under llvmpipe.

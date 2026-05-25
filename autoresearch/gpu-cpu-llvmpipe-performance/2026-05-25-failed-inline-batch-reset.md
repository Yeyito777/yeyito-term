# Failed experiment: inline batch length reset

## Hypothesis

Every GPU frame resets eight batch lengths. Replacing eight calls to the tiny
`gpubatchclear()` helper with direct `len = 0` stores in `gpubatchreset()` might
avoid call/setup overhead or produce simpler code in the frame-start hot path.

## Patch summary

In `render/gpu.c`, the experiment changed `gpubatchreset()` from eight
`gpubatchclear(&gpu.<batch>)` calls to direct assignments:

```c
gpu.bg.len = 0;
gpu.text.len = 0;
gpu.ctext.len = 0;
gpu.deco.len = 0;
gpu.obg.len = 0;
gpu.otext.len = 0;
gpu.octext.len = 0;
gpu.odeco.len = 0;
```

No renderer fallback, `gpudraw` bypass, renderer switch, llvmpipe detection trick,
text/emoji change, fractional scaling change, batching semantic change, clear-color
cache change, cleared-background skip change, or accepted vimnav row guard change
was used.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/inline-batch-reset/result.json`

## Result versus accepted vimnav current-line guard

Accepted score: `0.870250`  
Experiment score: `0.863580`  
Relative score: `0.992x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8330 | 1.6858 | 1.8503 | 0.992x | 1.029x | 0.999x |
| repaint | 0.8400 | 1.2168 | 1.8306 | 0.992x | 1.014x | 1.000x |
| scroll_ascii | 1.0018 | 0.9829 | 1.8639 | 1.016x | 0.997x | 0.999x |
| scroll_unicode | 0.9272 | 1.0732 | 1.8290 | 0.974x | 1.028x | 1.000x |
| scroll_emoji | 1.1226 | 0.7727 | 1.8680 | 0.990x | 1.001x | 0.998x |

## Decision

Rejected and reverted.

The direct stores helped ASCII wall but regressed cursor, repaint, unicode, and
emoji wall ratios. The total score remained below accepted, so keep the helper-call
form that the compiler already optimizes adequately.

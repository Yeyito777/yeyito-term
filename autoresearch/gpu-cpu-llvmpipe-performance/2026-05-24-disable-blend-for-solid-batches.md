# Accepted experiment: disable blending for solid GPU batches

## Hypothesis

The GPU renderer enables blending globally and draws both textured glyphs and
solid background/decoration rectangles with blending active. Solid batches always
emit fully opaque vertices (`a = 1.0f`), so blending is unnecessary for those
batches. Under llvmpipe, avoiding blend work for the large background/deco
batches should reduce software rasterization CPU and wall time while preserving
text/emoji blending for textured glyph batches.

## Patch summary

In `render/gpu.c`, `gpudrawbatch()` now toggles blending by batch type:

```c
if (textured) {
    glEnable(GL_BLEND);
    ...
} else {
    glDisable(GL_BLEND);
    ...
}
```

Textured batches (`text`, `ctext`, `otext`, `octext`) still enable blending and
set the same blend functions as before. Solid batches (`bg`, `deco`, `obg`,
`odeco`) draw with blending disabled.

This is a real GPU renderer optimization: it keeps `gpudraw` enabled, uses the
same triangle batches, preserves fractional scaling, keeps the glyph atlas and
color emoji paths intact, and does not detect llvmpipe or fallback to Xft.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-blend-solid-batches/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-blend-solid-batches-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-solidnoblend-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/disable-blend-solid-batches-validate
```

## Validation result versus accepted triangle-batch state

Previous accepted score: `0.712339`  
Experiment validation score: `0.750395`  
Relative score: `1.053x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.6453 | 2.4641 | 1.8507 | 1.096x | 0.864x | 1.000x |
| repaint | 0.7445 | 1.4359 | 1.8286 | 1.054x | 0.935x | 0.999x |
| scroll_ascii | 0.9230 | 1.0804 | 1.8653 | 1.015x | 0.969x | 1.000x |
| scroll_unicode | 0.8845 | 1.1668 | 1.8263 | 1.039x | 0.939x | 1.000x |
| scroll_emoji | 1.0848 | 0.8269 | 1.8714 | 1.013x | 0.975x | 1.001x |

## Decision

Accepted.

The improvement reproduced on the validation run and improves the weighted score
substantially. It improves wall time for every workload, including the
high-priority cursor and repaint workloads, and also improves GPU CPU ratios for
all workloads. RSS is essentially unchanged. The change is small, renderer-local,
and semantically clean: opaque solid rectangles do not need blending, while
textured glyph and emoji batches keep the existing blending behavior.

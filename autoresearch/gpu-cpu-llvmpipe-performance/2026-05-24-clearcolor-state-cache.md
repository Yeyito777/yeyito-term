# Accepted experiment: cache GL clear color state

## Hypothesis

After the accepted default-background clear optimization, the llvmpipe path clears
nearly every presented frame. `xstartdraw()` therefore sends the same
`glClearColor()` state repeatedly when the terminal default background has not
changed. Caching the currently installed clear color in GPU renderer state and
skipping redundant `glClearColor()` calls may reduce software-GL state validation
work while preserving the full-frame clear and all renderer behavior.

## Patch summary

In `render/gpu.c`, added clear-color cache state to `Gpu`:

```c
int clearvalid;
float clearcolor[3];
```

In the GPU branch of `xstartdraw()` in `x.c`, changed the full-frame clear path
to update GL clear color only when the resolved clear color changes:

```c
gpucolor(IS_SET(MODE_REVERSE) ? defaultfg : defaultbg, bg);
if (!gpu.clearvalid || !gpucoloreq(gpu.clearcolor, bg)) {
    memcpy(gpu.clearcolor, bg, sizeof gpu.clearcolor);
    gpu.clearvalid = 1;
    glClearColor(bg[0], bg[1], bg[2], 1.0f);
}
glClear(GL_COLOR_BUFFER_BIT);
```

The clear itself is still performed when required, `tfulldirt()` behavior is
unchanged, and the renderer remains on the actual GPU path. This does not fallback
to Xft, disable `gpudraw`, detect llvmpipe to switch renderers, or bypass GPU
text/emoji/fractional-scaling behavior.

## Validation

- `make`
- `make test_gpu_regressions`
- `make test`

All passed.

## Benchmarks

Initial result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/clearcolor-state-cache/result.json`

Validation rerun with 9 iterations / 2 warmups:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/clearcolor-state-cache-validate/result.json`

Benchmark command shape:

```sh
LP_NUM_THREADS=1 autoresearch/gpu-cpu-llvmpipe-performance/benchmark_llvmpipe.py \
  --iterations 9 --warmups 2 \
  --name st-llvmpipe-clearcolor-cache-val \
  --out autoresearch/gpu-cpu-llvmpipe-performance/runs/clearcolor-state-cache-validate
```

## Validation result versus accepted cleared-background state

Previous accepted score: `0.863260`  
Experiment validation score: `0.865042`  
Relative score: `1.002x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8316 | 1.6752 | 1.8504 | 0.990x | 1.012x | 1.000x |
| repaint | 0.8380 | 1.2182 | 1.8300 | 1.008x | 0.990x | 0.999x |
| scroll_ascii | 0.9920 | 0.9947 | 1.8647 | 1.014x | 0.998x | 0.999x |
| scroll_unicode | 0.9506 | 1.0627 | 1.8278 | 1.016x | 0.992x | 1.000x |
| scroll_emoji | 1.1455 | 0.7736 | 1.8682 | 1.001x | 1.014x | 0.999x |

## Decision

Accepted.

The validated total score improves modestly and repaint plus all scrolling wall
ratios improve. Cursor wall/CPU move slightly in the wrong direction, but the
regression is small while the weighted score and repaint workload improve. The
change is simple renderer-local state caching and preserves all GPU-renderer
behavior.

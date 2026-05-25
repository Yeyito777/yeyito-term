# Failed experiment: combine -frename-registers with -fno-plt

## Hypothesis

`-frename-registers` became the accepted frontier under llvmpipe. A previous
standalone `-fno-plt` run was very close before validation, so combining it with
`-frename-registers` might preserve the register-renaming gain while reducing
external call overhead in X/GL/FreeType-heavy paths.

## Patch summary

In `config.mk`, the experiment changed:

```make
CFLAGS = -O3 -march=native -flto -frename-registers
```

to:

```make
CFLAGS = -O3 -march=native -flto -frename-registers -fno-plt
```

No renderer source behavior changed. The actual GPU renderer remained active with
text, emoji, fractional scaling, accepted triangle batching, solid no-blend,
textured alpha-test, clear-color cache, cleared-background skip, and accepted
vimnav row guard intact. The benchmark still compared same-source Xft `gpudraw=0`
against GPU `gpudraw=1` under Mesa llvmpipe; it did not fallback to Xft, bypass
`gpudraw`, switch renderers, or detect llvmpipe.

## Validation

- `make`
- `make test_gpu_regressions`

Both passed before benchmarking.

## Benchmark

Result:

- `autoresearch/gpu-cpu-llvmpipe-performance/runs/rename-no-plt/result.json`

## Result versus accepted `-frename-registers` frontier

Accepted score: `0.876115`  
Experiment score: `0.854121`  
Relative score: `0.975x`

| workload | wall ratio | CPU ratio | RSS ratio | wall vs accepted | CPU vs accepted | RSS vs accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cursor_updates | 0.8214 | 1.6371 | 1.8515 | 0.987x | 0.986x | 1.001x |
| repaint | 0.8015 | 1.2514 | 1.8307 | 0.932x | 1.068x | 1.000x |
| scroll_ascii | 0.9898 | 0.9539 | 1.8636 | 0.981x | 0.996x | 1.000x |
| scroll_unicode | 0.9470 | 1.1613 | 1.8279 | 0.983x | 1.109x | 0.999x |
| scroll_emoji | 1.1445 | 0.7595 | 1.8687 | 1.013x | 0.984x | 0.999x |

## Decision

Rejected and reverted.

The combination materially regressed the weighted score, especially repaint and
cursor wall time. `-frename-registers` alone remains the accepted frontier; do not
combine it with `-fno-plt` blindly.

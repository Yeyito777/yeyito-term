# Baseline: main GPU renderer under Mesa llvmpipe vs Xft

Total weighted score: `0.681802`

| Workload | Weight | GPU wall speedup | GPU CPU ratio | GPU RSS ratio |
| --- | ---: | ---: | ---: | ---: |
| cursor_updates | 0.35 | 0.5691 | 2.8940 | 1.9553 |
| repaint | 0.30 | 0.6602 | 1.6866 | 1.9330 |
| scroll_ascii | 0.15 | 0.8610 | 1.1756 | 1.9636 |
| scroll_unicode | 0.10 | 0.8538 | 1.2253 | 1.9270 |
| scroll_emoji | 0.10 | 1.0330 | 0.8758 | 1.9496 |

Interpretation: wall speedup above 1.0 means llvmpipe GPU is faster than Xft; CPU/RSS ratios below 1.0 are better.

Priority blockers are `cursor_updates` and `repaint`, which carry 65% of the weighted score and are both much slower under llvmpipe at baseline.

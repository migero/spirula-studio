# Densification strategies: LichtFeld Studio vs spirula

A comparison of LichtFeld Studio's training strategies (MCMC, ADC, IGS+) against
spirula's densification machinery, written after reading both codebases. Verdict
first, then the findings it rests on. Status: **future considerations** — nothing
here is scheduled.

## Verdict

| Idea | Change? |
|---|---|
| Error-weighted sampling in the MCMC branch | **Yes, worth porting** — the one clear win |
| Periodic opacity reset, flag-gated | Maybe — could un-stick far shells (sky domes) |
| Classic clone/split ADC mode | No — long-axis split is the deliberate replacement |
| IGS+ Canny edge score | No — the loss-map modes already cover it, more flexibly |

## LichtFeld's three strategies

All live in `src/training/strategies/` (`mcmc.cpp`, `adc.cpp`,
`improved_gs_plus.cpp`, selected by the `strategy` string via a factory).
Shared parameterization: log-space scales, sigmoid opacity, ExponentialLR decay.
Per-strategy preset hyperparameters in `src/core/parameters.cpp`.

- **MCMC** — relocation only, no clone/split. Dead splats (opacity ≤ 0.005 or
  degenerate quaternion) are re-drawn by multinomial sampling over survivors;
  growth adds 5% per refine the same way; noise is injected on means every
  step. Their refinement over gsplat's MCMC: the sampling weights are the
  per-Gaussian **max render error over views** (accumulated from
  `_densification_info` in backward, window reset every 2 refine intervals),
  not uniform weights.
- **ADC** — classic 3DGS clone (small, grad > 0.0002) / split (large, /1.6 with
  positional noise) / prune, opacity reset every 3000 by clamping at
  logit(2·prune_opacity) and zeroing Adam moments. Notable implementation
  detail: **soft deletion** — pruned splats only zero their quaternion and
  optimizer state, and new splats fill the free slots before appending, so the
  tensors never resize.
- **IGS+** — their own strategy, and the interesting one: a Taming-3DGS
  quadratic budget ramp to cap, candidates chosen by
  `normalized_error × (1 + 0.25 × Canny-edge score)` (edge score from an edge
  rasterizer over ~8% of views), then **long-axis splits** into a fixed
  capacity pool. Bigger LRs than MCMC, decaying to 10% (not 1%).

## What spirula has

- **MCMC branch** (`use_revised_densification=false`; `McmcRelocation.cu`,
  `Relocation.cu`): relocation, growth, scheduled noise LR, opacity/scale
  regularizers — the same gsplat lineage, but with **uniform/blanket
  sampling weights**. This is the gap.
- **Revised densifier** (the default, `EngineDensify.cpp`): score-driven
  split-only. The score is a per-pixel loss map (`densify_loss_map_mode`:
  ssim / s_cielab / flip / edge_aware / NMS variants) reduced by
  `densify_score_mode` with quantile clipping, optional world-gradient blend
  and a learned score (`docs/notes/learned-densify-score.md`). Splits are
  **long-axis** with a scheduled split-opacity k, plus an oversize channel;
  dead splats relocate; budget grows by `growth_factor` with a `growth_caps`
  schedule under `cap_max`.
- Pruning stricter than any LichtFeld strategy: opacity, MaxWeight half-life,
  unrendered-for-N-steps, screen/world size clips. Exponential (not stepped)
  LR decay. Beyond strategies entirely: trust-region Adam, quantized training,
  PCA surfel regularizer, progressive resolution/splat budget, PPISP,
  bilagrid, tile skipping.

So spirula's default path is **at or past IGS+**: same long-axis split +
importance sampling shape, with a richer error statistic than their error +
Canny score.

## What each verdict rests on

- **Port error-weighted MCMC sampling.** Spirula already computes per-splat
  densify scores in the revised path; the same statistic can feed the
  multinomial draw in `McmcRelocation.cu` (relocation targets and growth
  sampling both). Small, self-contained, clear upside on the strategy the
  revised densifier does not replace (MCMC is the fallback for scenes where
  split-only struggles, and is what gsplat users expect).
- **Opacity reset, flag-gated.** Spirula has no periodic reset; the
  split-opacity schedule substitutes. But a reset (clamp raw opacity,
  zero Adam moments) is orthogonal and sometimes un-sticks shells that sit
  far from every camera with weak gradients — sky domes on fisheye rigs are
  the motivating case. Gated off by default; worth one experiment before
  keeping.
- **No ADC clone/split.** IGS+ demonstrates long-axis split ≥ clone/split;
  ADC is the legacy path, and adding it would be a second densification
  implementation to keep parity-tested.
- **No Canny edge score.** `densify_loss_map_mode=edge_aware` (and the
  learned score) already inject edge information into the splat score with
  more tunability than a single Sobel/NMS pass.

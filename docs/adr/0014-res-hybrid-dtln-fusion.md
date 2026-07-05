# ADR-0014: Phase-3 RES — parallel DTLN/AEC3 hybrid with echo-likelihood fusion

**Status:** Accepted.
**Date:** 2026-07-05
**Resolves:** the GB/T 45314 §5.7 double-talk blocker (ADR-0013 verdict, three corpora).
**Deviates from:** ADR-0001 (RES stage order), ADR-0007 (neural runtime) — both deviations measured and recorded below.

## Context

GB/T 45314-2025 §5.7 requires send-side double-talk grade 2b: a driver
speaking 6 dB below the downlink echo must survive the chain. Static AEC3
suppressor tuning cannot deliver it — measured ceiling with maximal
transparency (NS bypass, AGC off): near-end correlation **0.579** vs the
0.60 floor, and every transparency gain trades 1:1 against echo-clause
floors (TCL, §5.5.3 path-change stability). Full matrix in
`docs/phase-3-res-hybrid-notes.md`.

## Decision

Add a neural echo-control branch and fuse it with the AEC3 path per frame:

- **A path (echo owner):** Beamformer → AEC3, WebRTC-default suppressor
  config. Owns TCL, convergence profile, and path-change stability.
- **B path (near-end owner):** `DtlnResAdapter` — vendored DTLN-AEC
  (MIT) over the **raw mic + loopback**, 512/128 overlap-add, two stacked
  stateful-LSTM stages via ONNX Runtime. Preserves a near end 6 dB under
  echo (−1.25 dB, corr 0.731 standalone) but only attenuates echo ~16 dB.
- **Selector:** echo likelihood = max lagged correlation of render vs
  B-output log-power sequences (500 ms window, ≤250 ms lag), mapped to a
  crossfade weight α (rails 0.40/0.60, attack 0.6 / decay 0.1), plus an
  independent **echo veto** — post/pre-AEC3 amplitude ratio < 0.03 for
  3 consecutive frames forces the A path at attack speed. Render idle →
  A path, and the NS blend gate returns to VAD authority.
- **Alignment:** the A path is delayed 384 samples (24 ms) to match the
  B path's OLA latency; the eval strips this known offset before
  per-frame metrics (fixed latency is a §5.1 budget item, not damage).
- **NS coupling:** in RES mode the fusion weight drives the RNNoise
  blend gate (B-selected = near end → blend opens; A-selected = echo →
  full mopping).

Why not the alternatives (all measured, 2026-07-05): AEC3→DTLN cascade
destroys the near end (−24.8 dB — the model was trained on raw mic, not
residuals); DTLN alone fails TCL (16 dB); DTLN→AEC3 breaks the linear
filter (convergence 21–32 dB vs 40 floor).

## Runtime: ONNX Runtime, not TFLite (ADR-0007 deviation)

No packaged TensorFlow-Lite C library exists on any platform we build on
(macOS brew, ubuntu CI, Yocto aarch64). ONNX Runtime is packaged on all
three (brew bottle; upstream linux x64/aarch64 release tarballs —
`ORT_HOME` env in CMake). Models are deterministic conversions of the
vendored TFLite artifacts (`reference/convert_dtln_res.py`, equivalence
≤ 1.3e-4 over a 200-block stateful run) distributed as v0.4.2 release
assets, SHA256-pinned by `reference/fetch_res_models.py`. Build is
optional: no onnxruntime → RES stage compiles out, chain unchanged.

## eCall release-candidate preset

```
--res-models models --res-units 256 --ns-vad-blend 0.20,1.0 --no-agc
```

AGC2 off for the eCall profile: the software gate has no SLR floor (SLR
is a lab item per ADR-0013), and AGC's adaptive gain trajectory costs
0.10–0.14 of §5.7 correlation (0.648 → 0.513 measured). The v0.4.1
AGC-on production default for normal calls is unchanged — eCall (AECS)
is a distinct call profile.

## Measured results (RC preset, real-stimuli conditions, deterministic)

| Gate | Verdict |
|---|---|
| GB/T 45314 pre-compliance (5 conditions) | **floors met** (exit 2 — WARN, headroom targets pending): DT delta −10.5 dB (floor −12), corr 0.656 (floor 0.60), TCL 50.3–51.1 dB (floor 46), convergence profile green, §5.5.3 variation < 6 dB, B2 PASS |
| AEC-Challenge perceptual corpus (ADR-0012) | **PASS** — DT improves over the AGC-on default (aecmos_dt p50 3.28 → 3.90, dnsmos_sig 3.02 → 3.19); ST_NE/ST_FE floors green |

## Consequences and open items

1. **A55 CPU:** host RTF 0.071 with RES-256 (vs 0.020 without) → hot on
   the A55 (~70–100 % of one core projected). Levers, in order: ORT
   int8 dynamic quantization of the LSTM stages, render-gated inference
   (B path only while downlink is audible — the fusion already forces A
   when idle), dtln_aec_128 (corr 0.616 — thin margin). This is the
   next perf-loop item; the qemu cross-build re-measure folds into it.
2. Selector constants were calibrated on the synthetic-deterministic +
   real-stimuli GB conditions and the AEC-Challenge corpus; re-calibrate
   against Phase-2 cabin recordings when vehicle access lands.
3. Eval-harness changes that ride along: lag-compensated near-end
   metrics (+ `near_end_lag_ms` column), 60 dB ERLE measurement ceiling,
   RES latency strip, checker BLOCK on empty CSV (was a vacuous GREEN).
4. Lab-only items unchanged from ADR-0013 (HATS/POI, P.863, TS 103 558/
   103 802, certified DT grading) — this gate remains pre-compliance,
   not certification.

# Phase-3 RES — hybrid topology findings (2026-07-05)

Working notes for the GB/T 45314 §5.7 double-talk blocker. Measurements on
the real-stimuli GB/T conditions (P.501-lineage speech + DEMAND TCAR),
DTLN-AEC pretrained models (vendor/dtln-aec, MIT) converted to ONNX by
`reference/convert_dtln_res.py` (numerical equivalence ≤ 1.3e-4 vs TFLite).

## Why suppressor tuning cannot clear §5.7 (measured 2026-07-05)

- AEC3 nearend-transparency sweep (detector + LF/HF masks, `--aec3-tune`):
  DT near-end level delta improves −33.3 → −6.8 dB, but correlation
  saturates at 0.49 (floor 0.60). AEC-only ceiling (NS bypass, AGC off,
  maximal transparency): **0.579** — the suppressor trades echo leakage
  against near-end damage 1:1 at 6 dB NE-under-echo; no static config wins.
- Detector dichotomy: relaxed detector → DT −6.8 dB but path-change
  reconvergence reads as "nearend" → §5.5.3 variation 8.46 dB (> 6);
  strict detector → §5.5.3 passes (5.15) but DT −18.5 dB. Zero-sum.

## Topology matrix (offline, DTLN-AEC-256 unless noted)

| Topology | DT delta | DT corr | FE atten (pre-NS) | Verdict |
|---|---:|---:|---:|---|
| AEC3 tuned alone (best static) | −6.8 | 0.49 | ~30 dB (+NS→50) | corr wall |
| AEC3 → DTLN (ADR-0001 order) | −24.8 | 0.56 | 55 dB | kills NE — DTLN trained on raw mic, not residuals |
| DTLN alone (raw mic) | **−1.25** | **0.731** | 15–16 dB | fails TCL 46 |
| DTLN → AEC3 (R3) | — | 0.36 | conv 21–32 dB | AEC3 can't adapt behind nonlinear front-end; §4.8.4 + §5.5.3 fail hard |
| **Parallel hybrid, selector fusion** | **−4.3** | **0.649** | 30 dB (+NS mop) | **only viable path** |

Parallel hybrid: mic → {A: AEC3 default … echo owner; B: DTLN(mic, ref) …
near-end owner}; per-frame crossfade by echo-likelihood of B's output
(lagged log-power-sequence correlation vs render, 500 ms window). B path
latency ≈ 24 ms algorithmic (512/128 OLA) + hop buffering; A path gets a
matching delay line before fusion.

## In-chain design decisions (from prototype misses)

1. **Render idle → force A** (B2 stability was 9.7 dB vs 10 limit with
   all-B; existing A+NS behaviour passes at ~3–4 dB — and DTLN inference
   can be skipped entirely: CPU only spent while downlink is audible).
2. **Bias selector toward A on ambiguity** (thr window high) — §5.5.3
   variation is the sensitive clause; FE/TV leakage through B is capped by
   NS mopping only ~14 dB beyond B's own 16.
3. NS echo gate couples to the selector (gate ≈ alpha): B-selected frames
   are near-end — blend opens; A-selected frames get full RNNoise mopping.
4. Corr budget: 0.649 pre-NS/AGC vs 0.60 floor; AGC measured cost ≈ 0.09.
   Levers if the end-chain number lands short: harder switching (less
   mid-fade A/B mixing), dtln_aec_512, AGC preset tuning for eCall RC.

## Runtime decision (ADR-0007 deviation → ADR-0014)

ONNX Runtime, not TFLite: no packaged TFLite C library exists on any
deploy platform (macOS brew, ubuntu CI, Yocto aarch64), ORT is packaged on
all three (brew bottle 1.27; upstream linux x64/aarch64 release tarballs,
SHA-pinnable). Models converted offline, bit-provenance to the vendored
MIT artifacts; distribution via GitHub release assets + SHA-pinned fetch
script (repo policy: models not committed).

Model SHAs (dtln_aec_*.onnx, opset 13, tf2onnx 1.17.0) — pinned in
`reference/fetch_res_models.py`, distributed as v0.4.2 release assets:
- 128_1 `8234b843827701d64e5f84541b86aa658942b43f04d432883b732c3812d4c3e7`
- 128_2 `3fd992cb89034cbe50fe4be7f0bb914129caba223862101fec5824611ba00b9b`
- 256_1 `602fd28d6f8a6566dc6daf5f453eeeefc8acf46cadf4ef03cd13e0f2a609f29d`
- 256_2 `36ca8c452ae751a7c87bdf5870f57bda8a4f0bf849491fe272bbe996e45ae7cf`

## Status — SHIPPED 2026-07-05 (ADR-0014)

In-chain results at the eCall RC preset (RES-256, vad 0.20/1.0, AGC off):
**GB/T 45314 gate floors met** (exit 2; DT delta −10.5 / corr 0.656 /
TCL 50.3–51.1 / §5.5.3 < 6 dB / B2 PASS), AEC-Challenge corpus PASS with
DT improved (aecmos_dt p50 3.90). Deterministic across reruns.

Selector calibration lessons (trace-driven, `ECNR_RES_TRACE`):
- Likelihood window must stay ≥ 500 ms — 300 ms reads the syllabic
  envelope of ANY two speech signals as echo (NE frames hit lik p50 0.48).
- The ratio veto needs a 3-frame streak — NE frames dip under 0.03 for
  1–2 frames between syllables (ratio p10 0.013 on NE-active frames).
- Veto must close at attack speed or single-frame ERLE point samples in
  the convergence profile catch the lingering B path.
- Render idle: NS gate returns to VAD authority (α is not a verdict when
  no echo exists) — pinning it to α over-suppressed ST_NE clips.

CPU: host Release RTF 0.071 with RES-256 (0.020 without) → A55-hot;
next levers = ORT int8 dynamic quantization, render-gated inference,
128-unit fallback (corr 0.616, thin). Tracked in ADR-0014 consequences.

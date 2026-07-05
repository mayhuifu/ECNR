# GB/T 45314 eCall gate current verdict

## Re-verdict 2026-07-05 — **floors met** (RES hybrid, ADR-0014)

**Chain under test:** `main` with the Phase-3 RES hybrid (parallel
DTLN-AEC-256 / AEC3 fusion, ADR-0014) at the eCall RC preset:
`--res-models models --res-units 256 --ns-vad-blend 0.20,1.0 --no-agc`.

**Overall: WARN (exit 2) — all pre-compliance floors met; headroom
targets pending.** The double-talk blocker that three corpora pointed at
since 2026-05-27 is cleared; release to GB/T 45314 vehicle/lab validation
is no longer blocked by this gate.

| Condition | Result |
|---|---|
| DT driver −6 dB (§5.7) | **floors PASS** — near-end delta −10.5 dB (floor −12), corr 0.656 (floor 0.60), lag 24 ms; soft target −6 dB pending |
| Far-end quiet TCL/convergence (§5.5.1, §4.8.4) | PASS — TCL proxy 50.3 dB, convergence profile green (headroom target 55 dB pending) |
| Far-end B1 road (§5.8.1) | PASS (same headroom note) |
| Time-varying path (§5.5.3) | PASS — variation < 6 dB with the WebRTC-default A-path suppressor |
| B2 noise-only stability (§5.8.1) | PASS |

Cross-checks on the same binary + preset: AEC-Challenge perceptual gate
**PASS** with DT *improved* over the AGC-on default (aecmos_dt p50
3.28 → 3.90); 43/43 unit tests; verdict bit-identical across reruns.

Why this worked where suppressor tuning could not: the measured AEC3
transparency ceiling was corr 0.579 (< 0.60 floor) — spectral masking
cannot separate a near end 6 dB under echo. The hybrid gives the DT
frames to a neural branch trained for exactly that separation, and every
echo-owned clause stays on the untouched AEC3 path. Full topology
matrix + selector calibration data: `docs/phase-3-res-hybrid-notes.md`;
decision record: ADR-0014.

Open before vehicle validation: A55 CPU budget for the neural branch
(host RTF 0.071 — int8 quantization / render-gated inference queued),
headroom targets (55 dB steady ERLE, −6 dB DT soft target), and the
ADR-0013 lab-only items (unchanged).

---

## Historical: re-verdict 2026-07-04 — gate ported to main (v0.4.1+ chain, AGC-on default)

**Chain under test:** `main` post-v0.4.1 (AGC2 on by default per ADR-0012 A6, int8 RNNoise, rnnoise_default NS). `ecnr_eval` now also defaults AGC **on** so the gate grades the production configuration by default (`--no-agc` opts out).

**Overall: BLOCK** — unchanged from 2026-05-27, and for the same reason. The chain's behaviour is stable across the v0.3 → v0.4.1 arc (default-config numbers reproduce to 0.01 dB):

| Config | B2 noise range (<10 dB) | DT near-end delta (≥−12 dB) / corr (≥0.60) | Far-end TCL / convergence / time-varying | Overall |
|---|---:|---:|---|---|
| production default (AGC on) | 47.52 dB **FAIL** | −24.75 dB / 0.00 **FAIL** | 3× PASS | BLOCK |
| + `--ns-dry-blend 0.20` (eCall preset candidate) | **PASS** (9.0 dB) | −23.98 dB / −0.05 **FAIL** | 3× PASS (headroom WARN: steady ERLE 44.2 dB < 55 dB target) | BLOCK |
| `--no-agc` (pre-v0.4.1 legacy) | 48.09 dB FAIL | −39.96 dB / 0.00 FAIL | 3× PASS | BLOCK |

Reading:

- **B2 is solved by the known preset** (`--ns-dry-blend 0.20`), now re-confirmed on the AGC-on chain. Promoting it to an emergency-call preset remains a product decision (it trades ~36 dB of steady-state echo headroom, staying above the hard floors), pending validation on real cabin audio.
- **Double-talk remains the sole hard blocker** and is unchanged: stage-tap evidence (below, 2026-05-27) localizes the near-end loss to AEC3's nonlinear suppression, upstream of RNNoise and AGC. This is the third independent corpus pointing at the same gap (AEC-Challenge ST_FE structural dnsmos_sig, cabin-demo over-suppression, GB/T 45314 5.7 DT) — the Phase-3 neural RES work item.
- The perceptual AEC-Challenge gate (ADR-0012) stays **PASS** on the same binary — the two gates measure different things by design: AECMOS grades DT at ~3.3 (fair) on real conversational recordings; the GB proxy measures near-end level preservation against a −6 dB driver under continuous far-end, which is the harsher eCall-style condition.

The 2026-05-27 analysis below remains the authoritative diagnosis; nothing in v0.4.x moved it.

### Real-recording stimuli update (same day, later)

The conditions were upgraded to real recordings (P.501-lineage continuous speech + DEMAND TCAR road noise; ADR-0013 addendum). Same chain, real stimuli, −5.3 dB global crest-factor headroom offset:

| Config (real mode) | B2 noise range (<10 dB) | DT near-end delta (≥−12 dB) / corr (≥0.60) | Far-end conditions | Overall |
|---|---:|---:|---|---|
| production default (AGC on) | 34.5 dB **FAIL** | −33.3 dB / 0.08 **FAIL** | 3× PASS | BLOCK |
| + `--ns-dry-blend 0.20` | **PASS** | −19.4 dB / 0.11 **FAIL** | 3× PASS (headroom WARN) | BLOCK |

The verdict shape is identical to the synthetic fixtures — B2 solvable by the preset, double-talk the sole hard blocker — which retroactively validates the synthetic proxies and hardens the Phase-3 RES case: two independent stimulus families, one diagnosis.

---

## Original verdict 2026-05-27

**Date:** 2026-05-27
**Baseline:** `v0.3` / `0df3406`
**Branch under test:** `codex/gb45314-release-gate`

## Verdict

**BLOCK: do not release the current chain as GB/T 45314 eCall-ready.**

The baseline remains valid as the customer-passed basic ECNR implementation, but the stricter China emergency-call pre-compliance gate exposes release-blocking risks:

| Condition | Result | Blocking metric |
|---|---:|---|
| `ecall_farend_quiet_tcl_convergence` | PASS | Echo/TCL proxy and initial convergence pass |
| `ecall_farend_b1_road_convergence` | PASS | Echo/TCL proxy and initial convergence pass with road-noise proxy |
| `ecall_timevarying_path` | PASS | Time-varying echo degradation proxy passes |
| `ecall_b2_noise_only_stability` | FAIL | `noise_level_range_db = 47.52 dB`, limit `< 10 dB` |
| `ecall_doubletalk_driver_minus6` | FAIL | `near_end_level_delta_median_db = -24.75 dB`, floor `>= -12 dB`; `near_end_correlation = 0.00`, floor `>= 0.60` |

## Release Gap

The current implementation is good enough to preserve as the customer-passed basic ECNR baseline, but it is not yet a productive China eCall release candidate.

What is covered now:

- A reproducible GB/T 45314 eCall software pre-compliance gate exists.
- Far-end echo/TCL proxy, initial convergence, and time-varying echo-path proxy pass on the default synthetic fixtures.
- RNNoise is built in, and the gate can sweep the existing RNNoise blend controls.

What still blocks release:

- Default no-speech B2 output pumps by `47.52 dB`, which violates the `< 10 dB` noise-variation floor.
- `--ns-dry-blend 0.20` can reduce B2 variation to `9.02 dB`, but it is only a tuning candidate and reduces echo headroom.
- Double-talk remains damaged even with RNNoise bypassed. The remaining blocker is AEC3 nonlinear suppression / double-talk mode control, not just the RNNoise model.

No release tag should be created until the double-talk gate passes and the B2 preset is validated on real cabin recordings.

## Debug Update

The RNNoise blend sweep shows the B2 failure is tunable with the existing production knob:

| Config | B2 noise range | Far-end 5 s ERLE | Double-talk near-end delta |
|---|---:|---:|---:|
| `--agc` | 47.52 dB | 80.00 dB | -24.75 dB |
| `--agc --ns-dry-blend 0.10` | 2.99 dB | 47.09 dB | -30.18 dB |
| `--agc --ns-dry-blend 0.20` | 9.02 dB | 40.86 dB | -23.98 dB |
| `--agc --ns-dry-blend 0.25` | 9.12 dB | 38.88 dB | -22.18 dB |

`--ns-dry-blend 0.20` is the best measured compromise so far: it passes the B2 `< 10 dB` floor and keeps the hard far-end convergence floor intact, but it reduces echo headroom and does not fix double-talk. Higher dry blends improve near-end level but push echo ERLE below the 46 dB TCL proxy and/or the 40 dB convergence floor.

The remaining blocker is upstream of RNNoise: with `--ns-dry-blend 1.0`, which bypasses RNNoise, the double-talk near-end level is still heavily damaged before AGC. That points to AEC3 nonlinear echo suppression / double-talk mode control, not the RNNoise model alone.

## Stage-Tap Evidence

`ecnr_eval --stage-wavs` now writes per-stage WAVs so the double-talk loss can be localized without changing the production processing path.

Command:

```sh
./build/ecnr_eval \
  --run \
  --conditions /tmp/gbt45314_stage_conditions \
  --out /tmp/gbt45314_stage_default.csv \
  --stage-wavs /tmp/gbt45314_stage_default \
  --agc
```

Measured on `ecall_doubletalk_driver_minus6`:

| Stage | Near-end level delta | Raw correlation | Echo-removed correlation |
|---|---:|---:|---:|
| `post_bf` | `+8.56 dB` | `0.370` | `0.982` |
| `post_aec` | `-24.64 dB` | `-0.051` | `-0.051` |
| `post_ns` | `-39.96 dB` | `0.002` | `0.002` |
| `post_agc` | `-24.75 dB` | `0.002` | `0.002` |

The near-end speech is present after beamforming and is lost at AEC3 output before RNNoise. A simple RNNoise preset cannot close the double-talk gap.

An attempted WebRTC `mobile_mode` experiment was rejected: it fails far-end convergence and time-varying echo-path floors. A naive dry-mic blend during double-talk is also not release-safe because it risks leaking uncancelled far-end echo unless guarded by a validated double-talk detector.

## Commands

```sh
python3 reference/gen_gbt45314_ecall_conditions.py \
  --out-root /tmp/gbt45314_ecall_conditions

./build/ecnr_eval \
  --run \
  --conditions /tmp/gbt45314_ecall_conditions \
  --out /tmp/gbt45314_ecall.csv \
  --out-wavs /tmp/gbt45314_ecall_wavs \
  --agc

python3 reference/check_gbt45314_ecall_gate.py \
  --in-csv /tmp/gbt45314_ecall.csv
```

## Engineering Readout

The passing far-end cases say the WebRTC AEC3 backbone can remove deterministic cabin echo in quiet, road-noise, and time-varying echo-path conditions.

The failures are production-relevant:

- **B2 no-speech noise stability:** the chain output level pumps heavily in noise-only emergency-call mode at the default RNNoise setting. A 0.20 dry blend fixes the hard B2 floor in the synthetic gate, but it should be treated as an emergency-call preset candidate, not a full release decision.
- **Double-talk near-end preservation:** near-end speech is over-suppressed when far-end echo is present even when RNNoise is bypassed. This maps to GB/T 45314 5.7 and needs double-talk-aware AEC mode control or a neural residual/post-filter stage.

## Next Fix Candidates

1. Promote `--ns-dry-blend 0.20` into an explicit emergency-call preset candidate and validate it on customer cabin recordings.
2. Add double-talk-aware AEC mode control or replace the nonlinear suppression stage with a validated post-filter. RNNoise-only tuning cannot recover speech already removed by AEC3 nonlinear suppression.
3. Re-run the gate after each tuning change. Do not lower the gate unless a vehicle/lab recording proves the fixture proxy is too strict.

## Lab Caveat

This gate does not certify GB/T 45314. HATS SLR/RLR, POI calibration, formal P.863 MOS-LQO, ETSI listening-effort, ETSI echo-impairment, and certified double-talk class grading still require the vehicle/lab setup.

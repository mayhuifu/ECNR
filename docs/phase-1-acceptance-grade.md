# Phase 1 Acceptance Grade — First Run (2026-05-19)

> Per [ADR-0012](adr/0012-phase-1-acceptance-bar.md) Action Item #3: run the full gate on the current chain (`main` HEAD `4c6cc48`, post-AGC2 wiring) against the Phase-1 4-condition set, publish results. **This is the calibration run that turns the ADR's aspirational numbers into measured reality.**

## TL;DR

The gate **BLOCKS** with both AGC off and AGC on. Two failure axes dominate:

1. **DNSMOS-SIG / OVR fail by 1–2 MOS points** on real-voice conditions — the chain is damaging near-end voice quality, which lines up exactly with the user-reported "voice over-suppression on babble" artifact that Step A + Step B NS mitigations have only partially addressed.
2. **AECMOS-DT sits right at the 3.0 floor** — borderline on the synthetic conditions, just at the floor on the road-voice condition.

**ERLE is excellent everywhere** (52–80 dB true ERLE) — echo cancellation is working. The bottleneck is downstream: NS over-suppression of near-end voice.

A surprise: **AGC ON makes things slightly worse on the high-echo condition** (DNSMOS-BAK falls 3.96 → 2.85 on `mixed_loud_echo`), because AGC normalizes amplitude and brings the noise floor up with it. AGC's perceptual cost is real — needs further investigation before defaulting it on.

## Setup

| | |
|---|---|
| Chain version | `main` at commit `4c6cc48` (post-PR #7 / AGC2 + DNSMOS + AECMOS + ADR-0012) |
| Eval tool | `ecnr_eval --run` with new `--out-wavs` and `--agc` flags |
| MOS tool | `reference/score_mos.py` with both ONNX models |
| Gate tool | `reference/check_acceptance_bar.py` enforcing ADR-0012 §2 |
| Conditions | 2 real-voice conditions built from existing fixtures (see below) |

The Phase-1 4-condition set per [ADR-0012 §3](adr/0012-phase-1-acceptance-bar.md) called for 1 synthetic + 3 real-recorded. Today we have 2 real-voice. Adding the third real-voice fixture is queued; synthetic conditions are dropped from grading because they have no near-end voice and produce uninformative DNSMOS-SIG numbers (the model correctly identifies silence-after-AEC as "no good speech").

### Conditions

| Condition ID | Source | Duration | Content |
|---|---|---:|---|
| `mixed_loud_echo` | `reference/mixed_sound.wav` + `reference/reference_sound_to_be_eliminated.wav` | 40 s | Near-end voice + dominant far-end echo (mic is mostly echo content) |
| `road_voice_realsynth` | `reference/synth/test_mic_road.wav` + `reference/synth/ref.wav` | 10 s | Real recorded voice + cabin-IR-convolved echo + road noise |

`echo_only_mic.wav` for each was approximated by convolving `ref.wav` with `reference/synth/cabin_ir.wav` (a sub-optimal proxy — real cabin recordings will have proper silence-pass echo-only captures per the Phase-2 protocol).

## Measured numbers

### AGC OFF (current production default)

| Metric | Target | Floor | `mixed_loud_echo` | `road_voice_realsynth` |
|---|---:|---:|---:|---:|
| `erle_true_median_db` | ≥ 20 | ≥ 12 | **80.00** ✓ | **52.79** ✓ |
| `dnsmos_sig` | ≥ 3.5 | ≥ 3.0 | **2.91** ✗ floor | **1.54** ✗ floor |
| `dnsmos_bak` | ≥ 3.0 | ≥ 2.5 | **3.96** ✓ | **3.97** ✓ |
| `dnsmos_ovrl` | ≥ 3.0 | ≥ 2.7 | **2.48** ✗ floor | **1.41** ✗ floor |
| `aecmos_echo` | ≥ 4.0 | ≥ 3.5 | **4.61** ✓ | **4.65** ✓ |
| `aecmos_dt` | ≥ 3.5 | ≥ 3.0 | **2.97** ✗ floor | **2.90** ✗ floor |

### AGC ON

| Metric | Target | Floor | `mixed_loud_echo` | `road_voice_realsynth` |
|---|---:|---:|---:|---:|
| `erle_true_median_db` | ≥ 20 | ≥ 12 | **80.00** ✓ | **52.79** ✓ |
| `dnsmos_sig` | ≥ 3.5 | ≥ 3.0 | **2.70** ✗ floor (↓0.21) | **1.83** ✗ floor (↑0.29) |
| `dnsmos_bak` | ≥ 3.0 | ≥ 2.5 | **2.85** ✓ floor (↓1.11) | **3.97** ✓ |
| `dnsmos_ovrl` | ≥ 3.0 | ≥ 2.7 | **2.01** ✗ floor (↓0.47) | **1.66** ✗ floor (↑0.25) |
| `aecmos_echo` | ≥ 4.0 | ≥ 3.5 | **4.65** ✓ | **4.65** ✓ |
| `aecmos_dt` | ≥ 3.5 | ≥ 3.0 | **2.99** ✗ floor (↑0.02) | **2.90** ✗ floor (=) |

Both gate runs end with `BLOCK — Phase 1 not ready for lab`.

## Findings

### 1. ERLE is healthy — echo cancellation is not the bottleneck

True-ERLE medians of 52–80 dB across both conditions. AEC3 + the chain's structural placement is doing its job at the linear-echo layer. The audible quality problem is downstream.

### 2. DNSMOS-SIG is the failure mode, by a wide margin

Floors targeted at 3.0; we're at **2.7–2.9** on the loud-echo case, **1.5–1.8** on road-noise. The road-noise condition is dramatically worse — this is exactly the failure mode the user reported earlier this session (voice over-suppression on babble / road noise). The Step A + Step B NS mitigations did not close the gap perceptually.

DNSMOS-BAK passes everywhere — meaning background noise IS suppressed effectively. The trade is voice quality, not noise quality. RNNoise is doing its job too aggressively.

### 3. AECMOS-DT is borderline

Sits at 2.90–2.99 across the board, just under the 3.0 floor. The AECMOS doubletalk score is a synthetic mean of (echo_mos + deg_mos)/2 — and `aecmos_other` (which is the deg_mos directly) is ~1.3 across the board, dragging the combined dt score down. This is consistent with the DNSMOS-SIG signal: near-end degradation is the dominant failure axis.

### 4. AGC has perceptual side-effects

`mixed_loud_echo`'s DNSMOS-BAK fell from **3.96 → 2.85** with AGC on. The chain output is already quiet; AGC normalizes it up, but also amplifies the noise floor between speech bursts. The signal moves into a range where the noise becomes audible, hence the BAK drop.

AGC ON on `road_voice_realsynth` is a slight WIN on SIG/OVRL (likely because the very quiet output without AGC was below the DNSMOS-model-training range; AGC moves it into-range). Different mechanism, same lesson: **AGC on/off is not free.**

Practical implication: defaulting `--agc` ON for v0.4 would be premature without resolving the BAK regression on high-echo conditions. Holding it off-by-default.

### 5. The ADR-0012 targets need a modest re-calibration

The §2 numbers were "informed best guess against literature." Now that we have actual numbers, the **floors** in particular look defensible — every metric where the chain is healthy passes the floor with margin, and every metric where the chain has a real problem fails the floor by a meaningful gap. **The bar is doing its job.**

The **soft targets** (especially `dnsmos_sig` at 3.5) look optimistic for our content. Real-voice DNSMOS-SIG above 3.5 may be unattainable without:
- A neural RES post-filter (Phase 3 work per ADR-0007), OR
- Substantial RNNoise tuning back-off (which loses noise suppression), OR
- A different NS model entirely (DeepFilterNet, per the earlier RNNoise-vs-DTLN comparison).

Action: defer adjusting §2 targets until either Phase-2 cabin data is in hand OR Phase-3 RES is tried — the next round of measurements will tell us whether 3.5 is "wrong target" or "right target, chain needs more work."

## Next moves the data points us toward

In rough priority order:

1. **The `aecmos_other` deg_mos at ~1.3** is the loudest signal. Drill into which segments of which conditions are scoring badly — it should correlate with the NS-over-suppression artifact, and there's an obvious tunable axis (`--ns-vad-blend`) we can sweep against the gate.

2. **The AGC perceptual regression on high-echo conditions** needs root-cause work. Options: tune `gain_controller2.adaptive_digital.max_gain_db` (currently 50; lowering caps the noise-floor amplification), or add a noise-gate post-AGC. Half-day to scope.

3. **Phase 3 (neural RES)** is the architectural answer to the DNSMOS-SIG problem. The gate now gives us the perceptual oracle to evaluate against. Gated on Phase 2 cabin data per the existing ADRs.

4. **Wire the gate into CI** as a manual-trigger workflow. The Phase-1 grade should rerun on every PR that touches `src/pipeline/`; we just don't want to bake the model downloads into CI yet.

## Reproduction

```sh
# 1. Build models dir + generate real-voice conditions (one-time)
python3 - <<'PY'
# (see Phase-1 grade run scripts in scripts/ — TODO commit those)
PY

# 2. Run the gate
mkdir -p /tmp/gate_run
./build/ecnr_eval --run \
    --conditions conditions/real_voice \
    --out /tmp/gate_run/results.csv \
    --out-wavs /tmp/gate_run \
    # add --agc to compare with AGC on
python3 reference/score_mos.py \
    --in-csv /tmp/gate_run/results.csv \
    --out-csv /tmp/gate_run/results_with_mos.csv \
    --conditions conditions/real_voice \
    --out-wavs /tmp/gate_run \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx
python3 reference/check_acceptance_bar.py \
    --in-csv /tmp/gate_run/results_with_mos.csv
```

Exit code 1 = BLOCK, 2 = WARN, 0 = GREEN.

## References

- [ADR-0012](adr/0012-phase-1-acceptance-bar.md) — the bar this grade is measured against.
- [ADR-0011](adr/0011-aec3-tuning-methodology.md) — `ecnr_eval` harness that produces the ERLE columns.
- `reference/score_mos.py` — DNSMOS + AECMOS post-processor.
- `reference/check_acceptance_bar.py` — gate enforcement.
- [`docs/phase-2-cabin-recording-protocol.md`](phase-2-cabin-recording-protocol.md) — the recording protocol that produces the real Phase-2 grade corpus.

# GB/T 45314 eCall gate current verdict

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
2. Add double-talk-aware AEC mode control. RNNoise-only tuning cannot recover speech already removed by AEC3 nonlinear suppression.
3. Re-run the gate after each tuning change. Do not lower the gate unless a vehicle/lab recording proves the fixture proxy is too strict.

## Lab Caveat

This gate does not certify GB/T 45314. HATS SLR/RLR, POI calibration, formal P.863 MOS-LQO, ETSI listening-effort, ETSI echo-impairment, and certified double-talk class grading still require the vehicle/lab setup.

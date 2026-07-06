# ECNR Validation Report — chain v0.4.3 vs applicable standards

**Date:** 2026-07-06 · **Chain under test:** v0.4.3 (`e0fef03`), ADR-0014 RES hybrid
**eCall RC preset:** `--res-models models --res-units 256 --ns-vad-blend 0.20,1.0 --no-agc`
**Scope:** software pre-compliance on the offline harness. Lab certification items (HATS/POI hardware, P.863, certified grading) are explicitly out of scope and listed in §6.

## Executive summary

| Standard / bar | What it governs | Verdict for ECNR v0.4.3 |
|---|---|---|
| **GB/T 45314-2025** (road-vehicle hands-free & voice interaction, CN) | eCall speech quality clauses | **All hard floors met** (gate exit 2); headroom targets pending; robust across 3 vehicle-noise classes |
| **GB 45672-2025** (mandatory CN eCall/AECS, in force 2027-07-01) | Invokes GB/T 45314 for emergency-call speech quality | **Pre-compliance ready** — no software-measurable clause blocks release to vehicle validation |
| **ADR-0012 acceptance bar** (Microsoft AEC-Challenge real recordings, DNSMOS P.835 + AECMOS) | Perceptual quality, conversational scenarios | **PASS** on the pinned corpus *and* on 30 unseen clips (default config BLOCKs on unseen — RC preset generalizes, default does not) |
| **3GPP TS 26.131/26.132** (terminal acoustics) | Send loudness rating etc. | AGC-on default targets SLR; eCall preset runs AGC-off — **SLR verification moved to lab** (§6) |
| **ITU-T P.1100/P.1110** (hands-free in vehicles) | Method lineage of GB/T 45314 | Covered indirectly via the GB/T proxies; no separate gate |
| **ITU-T P.501 / P.835** | Test stimuli / scoring methodology | Adopted: P.501-lineage speech stimuli; DNSMOS is the P.835 ITU methodology in model form |
| **ETSI EG 202 396-1 / TS 103 224 / TS 103 281** | Standard noise DBs & test signals | EG 202 396-1 car noise used in validation; TS 103 281/558/802 remain lab items |

## 1. Method

- **Harness:** `ecnr_eval --run` (objective clause proxies, per ADR-0011/0013 CSV contract) gated by `check_gbt45314_ecall_gate.py`; `run_aec_challenge.py` (perceptual, DNSMOS P.835 + scenario-aware AECMOS) gated by ADR-0012 §2.1 v2 floors + §3.1 applicability matrix. Exit codes are the verdicts; no manual scoring.
- **Determinism:** the GB/T gate result is bit-identical across reruns; the CI gate reruns the perceptual bar on every push.
- **Data provenance:** every test asset is public and SHA256-pinned or scripted: Microsoft AEC-Challenge real recordings (GitHub LFS), ITU-T P.501-lineage speech, DEMAND TCAR/TBUS (Zenodo, CC BY 4.0), ETSI EG 202 396-1 binaural car noise (docbox Open). Survey with verified URLs: `docs/test-datasets-survey.md`.

## 2. GB/T 45314-2025 — clause-by-clause (eCall RC preset)

GB/T 45314-2025《道路车辆 免提通话和语音交互性能要求及试验方法》is the speech-quality reference that GB 45672-2025 invokes for China eCall. The software gate maps its clauses to objective proxies (mapping in ADR-0013; metric refinements in ADR-0014 §Consequences).

| Clause | Requirement | Software proxy | Floor | Measured | Verdict |
|---|---|---|---|---|---|
| 5.1 | Round-trip delay Trtd < 210 ms | implementation delay + RTF | ≤ 110 ms, RTF < 1 | 10 ms frame + 24 ms RES latency; RTF 0.07 (host) | **PASS** |
| 5.5.1 | TCL ≥ 46 dB (nominal volume) | true-ERLE median, echo-only pass | ≥ 46 dB | **50.3–51.1 dB** | **PASS** |
| 4.8.4 (via 5.5) | Convergence: 5 dB @ 0/200 ms, 20 dB @ 1 s, 40 dB @ ≥1.2 s | per-frame ERLE profile | as listed | profile green at all sample points | **PASS** |
| 5.5.3 | Echo-path change: performance swing ≤ 6 dB | steady − worst-1s ERLE | ≤ 6 dB | **< 6 dB** (was 8.5–11.8 during transparency-only tuning) | **PASS** |
| 5.7 | Double-talk send grade ≥ 2b (driver −6 dB under echo) | near-end level delta + lag-compensated correlation | ≥ −12 dB; ≥ 0.60 | **−10.5 dB; 0.656** (from −33 dB / 0.08 at v0.4.1) | **PASS** |
| 5.8.1 B1 | Road-noise scene behaviour | ERLE floors under road noise | as 5.5.1/4.8.4 | green | **PASS** |
| 5.8.1 B2 | No-speech noise variation < 10 dB | output level range | < 10 dB | stable | **PASS** |
| — | Headroom targets (not floors) | steady ERLE 55 dB; DT delta −6 dB | soft | 50.3 / −10.5 | pending (WARN) |

**Robustness across vehicle-noise classes** (same generator, road-noise source swapped; RC preset):

| B1 noise source | Class | Gate verdict |
|---|---|---|
| DEMAND TCAR (pinned baseline) | passenger car, city | floors met (exit 2) |
| DEMAND TBUS | city bus | floors met (exit 2) |
| ETSI EG 202 396-1 Fullsize_Car1 130 km/h | highway cruise | floors met (exit 2) |

Why the chain passes now: static AEC3 suppressor tuning was measured at a
0.579 correlation ceiling on §5.7 (spectral masking cannot separate a
near end 6 dB under echo). v0.4.3's ADR-0014 hybrid gives double-talk
frames to a neural branch (DTLN-AEC over raw mic + loopback) while the
stock AEC3 path keeps owning every echo clause; an echo-likelihood
selector + ratio veto fuses them per frame.

## 3. ADR-0012 perceptual bar — Microsoft AEC-Challenge real recordings

Scored with Microsoft's DNSMOS P.835 and AECMOS models; floors per
ADR-0012 §2.1 v2 with the §3.1 per-scenario applicability matrix.

**Pinned 30-clip corpus** (CI-enforced):

| Config | Verdict | DT sig/ovrl p50 | aecmos_dt p50 | aecmos_echo p50 |
|---|---|---|---|---|
| production default (AGC on, RES off) | PASS | 3.02 / 2.71 | 3.28 | 4.46 |
| eCall RC preset | PASS | 3.19 / 2.79 | **3.90** | 4.36 |

**Unseen 30-clip set** (fresh GUIDs, zero overlap — generalization test):

| Config | Verdict | DT sig/ovrl p50 | aecmos_dt p50 |
|---|---|---|---|
| production default | **BLOCK** (7/10 DT clips under sig floor) | 2.76 / 2.41 | 3.29 |
| eCall RC preset | **PASS** (all enforced metrics) | 3.17 / 2.82 | 3.89 |

Two conclusions: the hybrid's double-talk advantage **generalizes** to
data no tuning loop ever saw, and the v0.4.1 default's DT pass was
floor-marginal (calibrated on the pinned set). Recorded follow-up:
ADR-0012 v3 should either re-baseline DT floors on a larger pool or
promote the RC preset once the A55 budget closes.

Known structural note (unchanged, informational): ST_FE dnsmos_sig/ovrl
are excluded by the §3.1 matrix — no near-end content exists in far-end
single-talk for DNSMOS to score.

## 4. 3GPP TS 26.131 / 26.132 — terminal acoustics

The AGC2 stage exists to put uplink speech in the TS 26.131 hands-free
SLR band (v0.4.1 decision, ADR-0012 A6). The eCall RC preset disables
AGC because its adaptive gain trajectory costs 0.10–0.14 of §5.7
correlation and the GB/T software gate has no SLR floor. Consequences:

- Normal-call profile: AGC on, unchanged — TS 26.131 posture intact.
- eCall profile: SLR must be verified in the lab measurement chain
  (analog gain staging may satisfy it without AGC2); explicitly parked
  as a lab item in ADR-0014.

## 5. Method-anchor standards (adopted, not separately gated)

- **ITU-T P.501**: near-end/far-end stimuli in the GB/T conditions are
  P.501-lineage continuous speech (the Recommendation GB/T 45314 cites
  for test signals). The ITU signal database itself is login-walled; the
  verified free Mandarin alternative (ETSI TS 103 281 Annex E) is the
  recommended China-market upgrade.
- **ITU-T P.835**: DNSMOS is the P.835 (SIG/BAK/OVRL) methodology in
  neural-estimator form — the perceptual bar therefore inherits P.835
  semantics.
- **ITU-T P.1100/P.1110**: the vehicle hands-free method family GB/T
  45314 descends from; covered transitively by the GB/T proxies.
- **ETSI EG 202 396-1**: its binaural car-noise database (the classic
  hands-free noise reproduction set) supplied the 130 km/h highway
  variant in §2.

## 6. Not claimable from software — the path to certification

Per ADR-0013, the following remain lab-only and are **not** claimed:
HATS/POI acoustic-electrical calibration, SLR/RLR (TS 26.131 numbers),
receive-path response, ITU-T P.863 (POLQA) MOS-LQO, ETSI TS 103 558
listening effort, ETSI TS 103 802 echo impairment, certified §5.7
double-talk class grading, and everything requiring the physical cabin.

Engineering prerequisites before vehicle validation:
1. **A55 CPU budget for the RES branch** — host RTF 0.071 vs 0.020
   without; levers queued: ORT int8 dynamic quantization, render-gated
   inference, 128-unit fallback (ADR-0014 §Consequences).
2. Headroom targets (55 dB steady ERLE; −6 dB DT soft target).
3. Phase-2 cabin recordings to re-calibrate selector constants on real
   vehicle acoustics (gated on vehicle access).

## Appendix — evidence trail

- Gates: `check_gbt45314_ecall_gate.py` exit 2 (×TCAR/TBUS/ETSI-130),
  `run_aec_challenge.py` exit 0 (pinned + unseen at RC preset); CI green.
- Decision records: ADR-0012 (perceptual bar), ADR-0013 (+addendum,
  clause mapping & real stimuli), ADR-0014 (RES hybrid).
- Data: `docs/test-datasets-survey.md` (URL-verified);
  `docs/validation-extended-2026-07-05.md` (fresh-data methodology).
- Chain topology findings: `docs/phase-3-res-hybrid-notes.md`.

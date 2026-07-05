# Extended validation on fresh web-sourced audio — 2026-07-05

Post-v0.4.3 robustness check of the ADR-0014 RES hybrid on data the chain
(and its tuning) had **never seen**. All audio downloaded fresh from the
public sources below and run through the standard harness
(`run_aec_challenge.py` / `ecnr_eval` + `check_gbt45314_ecall_gate.py`).
Companion dataset survey: `docs/test-datasets-survey.md`.

## 1. Unseen Microsoft AEC-Challenge real recordings (30 new clips)

Selection: next 10 alphabetical GUIDs per scenario from upstream
`datasets/real/` (GitHub trees API listing, 37,589 files), **zero overlap
with the pinned corpus**, `_with_movement` excluded, fetched via the same
LFS media URLs, ST_NE silence lpb synthesized — i.e., the pinned-corpus
recipe applied to fresh data.

| Config | Verdict | DT dnsmos_sig p50 | DT dnsmos_ovrl p50 | DT aecmos_dt p50 |
|---|---|---:|---:|---:|
| production default (AGC on, RES off) | **BLOCK** (7/10 DT clips below sig floor) | 2.76 | 2.41 | 3.29 |
| eCall RC preset (RES-256 + vad 0.20/1.0 + no AGC) | **PASS** (all enforced metrics green) | 3.17 | 2.82 | 3.89 |

Readings:
- The RES hybrid **generalizes**: its double-talk advantage reproduces on
  recordings that never touched any tuning loop (pinned-set DT deltas
  match: sig 3.02→3.19, aecmos_dt 3.28→3.90 there).
- The unseen set also exposes that the v0.4.1 default config sat **at the
  DT floor boundary**: the pinned set passed at sig 3.02 vs floor 3.00;
  fresh clips fall to 2.76. The ADR-0012 floors were calibrated on the
  pinned set — its DT PASS at defaults was marginal, not robust. The RC
  preset passes both sets with margin. Follow-up recorded below.

## 2. GB/T 45314 gate across three real vehicle-noise classes (RC preset)

Same conditions generator, road-noise source swapped; everything else
(P.501-lineage speech, levels, offsets) identical.

| Road-noise source | Provenance | Gate verdict |
|---|---|---|
| DEMAND TCAR (city car, committed baseline) | Zenodo, CC BY 4.0 | **floors met** (exit 2) |
| DEMAND TBUS ch01 (city bus) | Zenodo, CC BY 4.0, fetched fresh | **floors met** (exit 2) |
| ETSI EG 202 396-1 Fullsize_Car1 130 km/h (highway, binaural→16k mono) | ETSI docbox Open | **floors met** (exit 2) |

All three: every hard floor green; the only reports are the standing
headroom targets (55 dB steady ERLE; −6 dB DT soft target). The verdict
is not an artifact of the TCAR recording.

## 3. Reproduction

```sh
# Unseen AEC-Challenge set: enumerate GUIDs via the GitHub trees API,
# exclude GUIDs present in datasets/aec_challenge/MANIFEST.tsv, download
# mic/lpb via media.githubusercontent.com, synthesize ST_NE silence lpb
# (reference/fetch_aec_challenge.py helpers), write a MANIFEST.tsv, then:
python3 reference/run_aec_challenge.py --bench ./build/ecnr_bench \
  --dnsmos-model models/dnsmos_p835.onnx --aecmos-model models/aecmos.onnx \
  --manifest <ext>/MANIFEST.tsv --datasets-root <ext>/data --out-dir <out> \
  [--no-agc --bench-flags "--res-models models --res-units 256 --ns-vad-blend 0.20,1.0"]

# Vehicle-noise variants: fetch the noise (Zenodo DEMAND zips; ETSI docbox
# needs a browser User-Agent), 16 kHz mono ch01, then regenerate conditions
# with the source swapped (gen module's TCAR_WAV / TCAR_OFFSET_S) and run
# ecnr_eval + check_gbt45314_ecall_gate.py at the RC preset.
```

## 4. Follow-ups this surfaced

1. **Default-config DT marginality** (unseen set BLOCKs at v0.4.1 default):
   either promote the RC preset toward the production default once the
   A55 budget for RES closes, or re-baseline the ADR-0012 DT floors
   against a larger clip pool. Decision belongs with ADR-0012 v3.
2. Candidate corpus additions (verified in the survey, in value order):
   ICASSP-2023 blind doubletalk clips (incl. `_with_movement_` +
   enrollment), ETSI EG 202 396-1 car noises at 80/100/130 km/h as pinned
   B1 speed scenes, TS 103 281 Annex E Mandarin speech as a China-market
   near-end source, DEMAND TMETRO/STRAFFIC, TS 103 224 multichannel car
   noise for beamformer testing (ADR-0004).

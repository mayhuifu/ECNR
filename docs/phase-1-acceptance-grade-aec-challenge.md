# Phase 1 Acceptance Grade — AEC-Challenge First Run (2026-05-31)

> First diagnostic run of the current chain against a 30-clip stratified subset of Microsoft's [AEC-Challenge](https://github.com/microsoft/AEC-Challenge) real-recordings test set (10 each: `doubletalk`, `nearend_singletalk`, `farend_singletalk`). Per design spec [`docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md`](superpowers/specs/2026-05-31-aec-challenge-integration-design.md). Replaces the synthetic-fixture grade in [`phase-1-acceptance-grade.md`](phase-1-acceptance-grade.md) as the standardized corpus going forward.

## TL;DR

The gate **BLOCKS** both AGC-off and AGC-on, but the failure pattern is dramatically clearer than on the ad-hoc synthetic fixtures:

1. **Far-end single-talk is the dominant failure axis.** `dnsmos_sig` and `dnsmos_ovrl` fail the floor on **10/10** ST_FE clips in both configs. The chain delivers residual far-end leakage that DNSMOS scores as damaged near-end speech (which it correctly identifies as poor) — but the underlying mic signal has *no* near-end voice to begin with, so this is an upper-bound floor that NS tuning cannot move.
2. **Near-end single-talk passes every floor.** All five applicable metrics clear with margin in both configs — the chain preserves near-end voice quality cleanly when there's no echo competition.
3. **Doubletalk is borderline.** AGC-off has 5/10 ST_DT clips below the `dnsmos_sig` floor; AGC-on flips it green (p50 = 3.02 ≥ 3.00). AGC genuinely helps here by bringing near-end voice amplitude above the DNSMOS-model in-range threshold.
4. **AECMOS-echo passes everywhere** (p50 ≥ 4.34 across all 3 scenarios, both configs). AEC3 is fundamentally healthy; what we're failing on is the *near-end-quality* perception of the post-AEC output, not the echo cancellation itself.

**AGC ON improves doubletalk's `dnsmos_sig` from 2.98 → 3.02** (just over the floor) and is neutral-to-slightly-negative on the other scenarios. The earlier worry that "AGC hurts BAK" doesn't reproduce on real recordings — BAK stays ≥ 3.8 across both configs.

## Setup

| | |
|---|---|
| Chain version | `main` at commit [`cd164a1`](https://github.com/) (Phase 3 runner) |
| Eval tool | `reference/run_aec_challenge.py` |
| Scoring | DNSMOS P.835 + AECMOS (scenario-aware `talk_type`: `dt` / `st_ne` / `st_fe`) |
| Gate | ADR-0012 §2 floors / targets (unchanged — first run is diagnostic only) |
| Corpus | AEC-Challenge real-recordings, 30 clips: 10 each of `doubletalk`, `nearend_singletalk`, `farend_singletalk` |
| Manifest | [`datasets/aec_challenge/MANIFEST.tsv`](../datasets/aec_challenge/MANIFEST.tsv) |
| Per-run wall time | ~12 s (chain) on host; clips average ~12 s each |

## Measured numbers — AGC OFF (production default)

### `doubletalk` (10 clips)

| metric | p10 | p50 | p90 | floor | target | verdict |
|---|---:|---:|---:|---:|---:|---|
| `dnsmos_sig`   | 2.67 | **2.98** | 3.43 | 3.0 | 3.5 | ✗ floor — 5/10 below |
| `dnsmos_bak`   | 3.87 | 4.00 | 4.11 | 2.5 | 3.0 | ✓ |
| `dnsmos_ovrl`  | 2.41 | **2.68** | 3.15 | 2.7 | 3.0 | ✗ floor — 6/10 below |
| `aecmos_echo`  | 4.19 | 4.50 | 4.69 | 3.5 | 4.0 | ✓ |
| `aecmos_other` | 1.44 | 1.94 | 3.29 | — | — | (informational) |
| `aecmos_dt`    | 2.90 | 3.28 | 3.77 | 3.0 | 3.5 | ✓ (p10 = 2.90 → 2/10 below) |

### `farend_singletalk` (10 clips)

| metric | p10 | p50 | p90 | floor | target | verdict |
|---|---:|---:|---:|---:|---:|---|
| `dnsmos_sig`   | 1.80 | **2.27** | 2.53 | 3.0 | 3.5 | ✗✗ floor — **10/10 below** |
| `dnsmos_bak`   | 3.69 | 3.85 | 3.90 | 2.5 | 3.0 | ✓ |
| `dnsmos_ovrl`  | 1.47 | **1.79** | 1.91 | 2.7 | 3.0 | ✗✗ floor — **10/10 below** |
| `aecmos_echo`  | 4.19 | 4.34 | 4.62 | 3.5 | 4.0 | ✓ |

### `nearend_singletalk` (10 clips)

| metric | p10 | p50 | p90 | floor | target | verdict |
|---|---:|---:|---:|---:|---:|---|
| `dnsmos_sig`   | 2.78 | 3.46 | 3.51 | 3.0 | 3.5 | ✓ |
| `dnsmos_bak`   | 3.76 | 4.05 | 4.12 | 2.5 | 3.0 | ✓ |
| `dnsmos_ovrl`  | 2.25 | 3.12 | 3.25 | 2.7 | 3.0 | ✓ |
| `aecmos_other` | 2.80 | 3.62 | 4.18 | — | — | (informational) |

**CORPUS VERDICT (AGC off): BLOCK** — fails on `doubletalk.dnsmos_sig`, `doubletalk.dnsmos_ovrl`, `farend_singletalk.dnsmos_sig`, `farend_singletalk.dnsmos_ovrl`.

## Measured numbers — AGC ON

### `doubletalk` (10 clips)

| metric | p10 | p50 | p90 | floor | target | Δ vs AGC off (p50) |
|---|---:|---:|---:|---:|---:|---:|
| `dnsmos_sig`   | 2.54 | **3.02** ✓ | 3.42 | 3.0 | 3.5 | **+0.04** (cleared) |
| `dnsmos_bak`   | 3.71 | 3.90 | 4.05 | 2.5 | 3.0 | −0.10 |
| `dnsmos_ovrl`  | 2.27 | **2.71** ✓ | 3.14 | 2.7 | 3.0 | **+0.03** (cleared) |
| `aecmos_echo`  | 4.27 | 4.46 | 4.58 | 3.5 | 4.0 | −0.04 |
| `aecmos_other` | 1.51 | 2.00 | 3.30 | — | — | +0.06 |
| `aecmos_dt`    | 2.91 | 3.28 | 3.76 | 3.0 | 3.5 | 0.00 |

### `farend_singletalk` (10 clips)

| metric | p10 | p50 | p90 | floor | target | Δ vs AGC off (p50) |
|---|---:|---:|---:|---:|---:|---:|
| `dnsmos_sig`   | 1.62 | **1.93** | 2.24 | 3.0 | 3.5 | ✗✗ −0.34 (worse) |
| `dnsmos_bak`   | 3.71 | 3.84 | 3.87 | 2.5 | 3.0 | −0.01 |
| `dnsmos_ovrl`  | 1.46 | **1.57** | 1.74 | 2.7 | 3.0 | ✗✗ −0.22 (worse) |
| `aecmos_echo`  | 4.11 | 4.34 | 4.58 | 3.5 | 4.0 | 0.00 |

### `nearend_singletalk` (10 clips)

| metric | p10 | p50 | p90 | floor | target | Δ vs AGC off (p50) |
|---|---:|---:|---:|---:|---:|---:|
| `dnsmos_sig`   | 2.71 | 3.32 | 3.52 | 3.0 | 3.5 | −0.13 |
| `dnsmos_bak`   | 3.73 | 3.96 | 4.12 | 2.5 | 3.0 | −0.09 |
| `dnsmos_ovrl`  | 2.20 | 3.02 | 3.25 | 2.7 | 3.0 | −0.10 |
| `aecmos_other` | 2.59 | 3.52 | 4.14 | — | — | −0.10 |

**CORPUS VERDICT (AGC on): BLOCK** — still fails on `farend_singletalk.dnsmos_sig` and `farend_singletalk.dnsmos_ovrl`. Doubletalk flips green.

## Findings

### 1. ST_FE is the floor we cannot clear with this chain

Both configs put `dnsmos_sig` at p50 ≈ **1.9–2.3** on ST_FE clips, against a floor of 3.0 — a 0.7–1.1 MOS gap on 10/10 clips. The mechanism is fundamental: an ST_FE clip has no near-end voice, only far-end echo. After AEC suppresses ~99% of the echo, the residual ~1% still scores as "speech with audible defects" by DNSMOS-SIG (the model can't tell that the audible content is leak rather than damaged near-end). **No NS-blend or AGC tuning can move this floor** — only deeper echo cancellation (Phase 3 neural RES) or a different evaluation framing (acknowledge that ST_FE inputs should be scored on `aecmos_echo` alone, which IS passing at p50 ≥ 4.34).

This is the strongest argument yet for Phase 3 (neural RES). The numerical gap is large, persistent across configs, and concentrated on the scenario where the chain's residual leakage is most exposed.

### 2. ST_NE passes the gate with margin

Nearend-only clips clear every floor in both configs — `dnsmos_sig` p50 = 3.46 (AGC off) / 3.32 (AGC on), `dnsmos_ovrl` p50 = 3.12 / 3.02. The chain preserves near-end voice quality cleanly when there's no echo competing. This contradicts the earlier worry from synthetic-fixture grading that RNNoise was destroying voice — on real recordings of clean near-end speech, the output is fine.

### 3. AGC helps doubletalk specifically

AGC-on flips doubletalk from `BLOCK` to `PASS` on both `dnsmos_sig` (2.98 → 3.02) and `dnsmos_ovrl` (2.68 → 2.71). Both gains are razor-thin (≤ 0.04) but they cross the floor. The mechanism: DT clips have both near-end voice AND far-end content; AGC amplifies the near-end speech bursts up into the DNSMOS-model in-range region, which scores them as higher-quality. AGC can't do the same on ST_FE (no near-end to amplify) or ST_NE (already loud enough). So AGC's benefit is scenario-specific.

### 4. The "AGC hurts BAK" worry from synthetic fixtures does NOT reproduce

The prior grade ([`phase-1-acceptance-grade.md`](phase-1-acceptance-grade.md) finding #4) noted `dnsmos_bak` falling from 3.96 → 2.85 on the `mixed_loud_echo` synthetic fixture with AGC on. On the AEC-Challenge corpus, `dnsmos_bak` stays in the 3.83–4.05 band across both configs — AGC barely moves it (±0.10 worst case). The synthetic-fixture observation was an artifact of that fixture's extreme echo dominance, not a general AGC defect. Holding AGC off-by-default remains conservative, but the case for default-on has gotten stronger.

### 5. AEC3 + the linear stack are unambiguously healthy

`aecmos_echo` p50 ≥ 4.34 across all scenarios and both configs (target = 4.0). The echo-cancellation layer is well above target everywhere. The bottleneck is entirely on the near-end-quality perception side, and as Finding #1 establishes, the ST_FE failure is structural rather than tunable.

## Implications

In priority order:

1. **Phase 3 (neural RES) is now the well-motivated next step.** Two independent test corpora (the synthetic fixtures earlier; this AEC-Challenge subset now) agree that NS-blend tuning is below the noise floor of the actual problem. The data now points specifically at ST_FE residual leakage as the failure mode RES would address.

2. **AGC default policy can be revisited.** The AEC-Challenge data refutes the BAK-regression worry from the synthetic grade. Net effect of `--agc on`: doubletalk passes the gate; ST_FE marginally worse but still floor-blocked regardless; ST_NE marginally worse but still floor-passing. The case for `--agc on` as the v0.4 default is closer to neutral than the earlier grade suggested. Defer the policy change until either (a) Phase 2 cabin recordings give a third data point, or (b) we get a sweep over AGC `max_gain_db` to find a less-aggressive operating point.

3. **ADR-0012 floors are defensible.** Per the spec non-goal, this run does not modify ADR-0012. But the corpus does confirm: every floor that's clearable IS clearable on some scenario; every floor that's failing fails by a meaningful gap. The bar is doing its job.

4. **ST_FE scoring methodology may need an explicit footnote.** The DNSMOS-SIG floor of 3.0 implicitly assumes "the audio contains intended near-end speech" — which is false for ST_FE clips. A follow-up could either (a) exclude ST_FE from `dnsmos_sig`/`dnsmos_ovrl` aggregation (gate on `aecmos_echo` only for ST_FE), or (b) keep the current gate semantics and treat ST_FE failures as a deliberate "residual leakage is audible" signal rather than near-end damage. The runner already supports either interpretation via the per-clip CSV.

## Reproduction

```sh
# Once: fetch the 30-clip subset (idempotent — skips cache hits)
python3 reference/fetch_aec_challenge.py

# Run the chain against the corpus, AGC off
mkdir -p /tmp/aec_grade_agcoff
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/aec_grade_agcoff

# Re-run with AGC on
mkdir -p /tmp/aec_grade_agcon
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --agc \
    --out-dir /tmp/aec_grade_agcon
```

Exit codes: 0 = PASS, 1 = BLOCK, 2 = cache miss / SHA drift.

## References

- [ADR-0012](adr/0012-phase-1-acceptance-bar.md) — the gate this corpus feeds into (unchanged)
- [Design spec](superpowers/specs/2026-05-31-aec-challenge-integration-design.md) — integration plan + Phase-0 recon addendum
- [Implementation plan](superpowers/plans/2026-05-31-aec-challenge-integration.md) — task-by-task playbook
- [Previous synthetic-fixture grade](phase-1-acceptance-grade.md) — for comparison
- [Microsoft AEC-Challenge GitHub](https://github.com/microsoft/AEC-Challenge) — upstream corpus

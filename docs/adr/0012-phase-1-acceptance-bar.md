# ADR-0012: Phase-1 acceptance bar — internal numeric targets before 3GPP TS 26.131 cert

**Status:** Accepted (provisional — v1 numbers were aspirational against literature; **v2 (2026-05-31) recalibrates against measured baseline** from the AEC-Challenge 30-clip subset + NS / AGC sweeps. Phase-2 cabin recordings will drive the next sharpening cycle).
**Date:** 2026-05-19 (v1); recalibrated 2026-05-31 (v2 — see §2.1).
**Resolves:** Open assumption A2 from [ADR-0011](0011-aec3-tuning-methodology.md) ("a near-end damage metric will be specified later") **plus** the broader question of "what numeric quality target are we trying to hit before booking 3GPP TS 26.131 lab time."
**Builds on:** [ADR-0011](0011-aec3-tuning-methodology.md) `ecnr_eval` harness; `reference/score_mos.py` (DNSMOS + AECMOS columns); `reference/run_aec_challenge.py` (AEC-Challenge corpus runner).

## Context

Phase 1 closeout has been ambiguous on one critical question: **how do we know the chain is "good enough" to be worth measuring against the carrier-cert spec?** Booking 3GPP TS 26.131 lab time costs $30–80K per cycle (per the standards review). Going in blind — without a defensible internal pre-cert pass — wastes that money on iteration that could have happened at the desk.

This ADR locks the **internal acceptance bar**: a per-metric numeric target on metrics we can measure today (no HATS, no anechoic chamber, no carrier sandbox), graded across a small but representative set of test conditions. Passing this bar is the **green light** for the next cycle of work (lab booking, real device validation), not the cert itself.

What this ADR is **not**:
- It is not the 3GPP TS 26.131 cert spec. Those numbers are external; this is internal.
- It is not a tuning prescription. It's the criterion against which tuning is measured.
- It is not a promise of pass at the lab. It's the floor that makes lab booking worth doing.

## Decision

### 1. The metric panel (six numbers per condition).

The acceptance bar runs on the same `ecnr_eval --run` CSV that the rest of the eval pipeline produces, augmented by `reference/score_mos.py`. Six load-bearing columns:

| Column | Source | Range | What it measures |
|---|---|---|---|
| `erle_true_median_db` | ADR-0011 §1 (echo-only chain pass) | dB | echo cancellation, oracle-truthful |
| `dnsmos_sig` | DNSMOS P.835 SIG | 1–5 | speech quality alone (replaces "PESQ on near-end") |
| `dnsmos_bak` | DNSMOS P.835 BAK | 1–5 | background-noise tolerability |
| `dnsmos_ovrl` | DNSMOS P.835 OVRL | 1–5 | overall perceived quality |
| `aecmos_echo` | AECMOS echo | 1–5 | residual echo perception (live once AECMOS wired) |
| `aecmos_dt` | AECMOS doubletalk | 1–5 | doubletalk preservation (the artifact the user has hit) |

`aecmos_other` is collected but not gated on at Phase 1 — it overlaps semantically with `dnsmos_sig`, which is the better-validated single source for near-end damage.

The acceptance bar is **independent per metric** — we don't aggregate to a single number. Aggregation across this panel obscures which axis is failing, and the failure axis is what drives the next sprint's tuning work.

### 2. Per-metric numeric targets (Phase 1 cut)

The targets are aspirational on a stand-in corpus (synthetic + the 5 real reference recordings already in `reference/`). Phase 2's 134-case corpus will likely require softening some of these; the next ADR re-cut will re-derive against measured data.

| Metric | Phase-1 target (median across conditions) | Phase-1 floor (worst single condition) | Rationale |
|---|---|---|---|
| `erle_true_median_db` | **≥ 20 dB** | ≥ 12 dB | TS 26.131 hands-free TCLw is 55 dB end-to-end including downlink + acoustic; ERLE alone hitting 20 dB on AEC3+RES means the chain contribution is reasonable. The 12 dB floor is the "chain is working at all" threshold. |
| `dnsmos_sig` | **≥ 3.5** | ≥ 3.0 | DNSMOS literature treats SIG ≥ 3.5 as "good speech quality preserved"; ≥ 3.0 as "speech is intelligible but degraded." Anything below 3.0 means voice is audibly damaged. |
| `dnsmos_bak` | **≥ 3.0** | ≥ 2.5 | Background-noise scores trend lower than SIG because real cabin noise is intrusive even after suppression. 3.0 = "noise audible but not distracting." |
| `dnsmos_ovrl` | **≥ 3.0** | ≥ 2.7 | Overall MOS aggregates SIG + BAK with a perceptual weighting; 3.0 is the "telephony intelligibility floor" referenced widely in the DNS-Challenge literature. |
| `aecmos_echo` | **≥ 4.0** | ≥ 3.5 | AECMOS is more permissive than human listeners on echo; we want a high bar here because echo is the most cert-visible failure mode. |
| `aecmos_dt` | **≥ 3.5** | ≥ 3.0 | Doubletalk is the artifact the user is currently observing (voice over-suppression on babble). Setting the floor at 3.0 forces us to address it before lab booking. |

If **any one metric** misses its floor on **any one condition**, Phase 1 is not "ready for lab." Soft targets (medians) are reported in the closeout note but are not block-on-failure.

### 2.1. Measured-baseline-informed numbers (v2, 2026-05-31) — AUTHORITATIVE

The v1 table above was best-guess against DNSMOS literature; with three diagnostic runs and two sweeps now in hand, several numbers were either too lenient (BAK/echo had margin to spare) or unreachable on certain scenarios (SIG/OVRL on far-end-single-talk). The recalibrated bar adds a **per-scenario applicability matrix** (§3.1) and adjusts floors/targets to match what the chain actually does today.

**Data sources** (all on `main` at commit `1a9544f` or later):
1. [`docs/phase-1-acceptance-grade.md`](../phase-1-acceptance-grade.md) — synthetic-fixture grade (AGC off + AGC on).
2. [`docs/phase-1-acceptance-grade-aec-challenge.md`](../phase-1-acceptance-grade-aec-challenge.md) — AEC-Challenge 30-clip first-run grade (AGC off + AGC on).
3. NS-blend corpus sweep ([commit `4b9acb5`](https://github.com/), 9 configs × 30 clips).
4. AGC max_gain_db corpus sweep ([commit `1a9544f`](https://github.com/), 7 configs × 30 clips).

**Recalibrated floors and targets** (apply only to scenarios where the metric is meaningful — see §3.1):

| Metric | v2 floor | v2 target | Change from v1 | Rationale |
|---|---:|---:|---|---|
| `erle_true_median_db` | ≥ 12 dB | ≥ 20 dB | unchanged | AEC-Challenge corpus doesn't include silence-pass echo-only oracles → metric is N/A for the AEC-Challenge gate. Continues to apply for `ecnr_eval --run` against `conditions/` trees with proper oracles. |
| `dnsmos_sig` | ≥ 3.0 | **≥ 3.3** | floor unchanged; **target lowered 3.5 → 3.3** | NE-only p50 = 3.46 (AGC off) — 3.3 is achievable; 3.5 is unreached in any measured config. DT borderline-passes at 3.0 floor only with AGC on. |
| `dnsmos_bak` | **≥ 3.0** | **≥ 3.5** | **floor raised 2.5 → 3.0; target raised 3.0 → 3.5** | Measured 3.85-4.05 across every config × scenario. v1 floor of 2.5 was lenient; 3.0 still passes everywhere with margin and tightens the regression-catcher. |
| `dnsmos_ovrl` | ≥ 2.7 | ≥ 3.0 | unchanged | DT-on-AGC p50 = 2.71 (just clears 2.7); NE p50 = 3.02-3.12 (just clears 3.0 target). v1 numbers happen to be exactly right for the corpus distribution. |
| `aecmos_echo` | **≥ 4.0** | **≥ 4.3** | **floor raised 3.5 → 4.0; target raised 4.0 → 4.3** | Measured 4.34-4.50 across the corpus. AEC3 is genuinely healthy; raising the floor catches future regressions without false-failing. |
| `aecmos_dt` | ≥ 3.0 | ≥ 3.5 | unchanged | DT corpus p50 = 3.28 (just under target, above floor). v1 numbers correct. |

**What v2 does not change:**
- Independent-per-metric scoring (no aggregation).
- The "floor miss = BLOCK" semantics.
- The v1 metric panel (six numbers per condition).

**Why these numbers and not others:**
- **Tightened where we have margin** (BAK, echo) so regressions show up faster.
- **Lowered SIG target** to a number we actually observe — 3.5 was aspirational against literature, not anchored in our content.
- **Left everything else** where measurement happens to land within ±0.05 of the v1 number.

### 3. The condition set Phase 1 grades against

Phase 1 closeout grades against a **synthetic + small recorded** set, not the full 134-case Phase-2 corpus. Justification: Phase 2 isn't started, but we still need a defensible internal bar for v1.

| Set | Source | Count | Purpose |
|---|---|---|---|
| Synthetic (CI fixture) | `conditions/synthetic/case_001_quiet_cabin` per the ADR-0011 generator | 1 | sanity baseline; chain-mechanics regression catcher |
| Real-mic recorded — small | `reference/synth/test_mic_road.wav` + a clean-near-end clip | 2 | the listening-test fixtures already in the repo |
| Real-mic recorded — fixture | `reference/mixed_sound.wav` + matched ref | 1 | the user-supplied "near-end voice + heavy echo" fixture |

That's 4 conditions for Phase 1 grading. **All four must pass the per-metric floor.** Phase 2 expands to 6 (per the recording protocol) and eventually 24 (4-take × 6 condition matrix) and eventually 134 (the full corpus).

The synthetic-fixture condition set above is **superseded** by the AEC-Challenge 30-clip subset for Phase-1 grading going forward:

| Set | Source | Count | Purpose |
|---|---|---|---|
| AEC-Challenge real-recordings subset | [`datasets/aec_challenge/MANIFEST.tsv`](../../datasets/aec_challenge/MANIFEST.tsv) — Microsoft AEC-Challenge real-recordings test set | 30 (10 each: DT / ST_NE / ST_FE) | Standardised perceptual-quality corpus going forward; runs via `reference/run_aec_challenge.py` |

The four original synthetic-fixture conditions stay graded under `phase-1-acceptance-grade.md` for historical comparison but are no longer the primary corpus.

### 3.1. Per-scenario applicability matrix (v2, 2026-05-31)

Not every metric is meaningful on every scenario. ST_FE clips have no intended near-end speech, so a low `dnsmos_sig`/`dnsmos_ovrl` score on an ST_FE clip reflects "residual far-end leakage is audible" — useful diagnostic, but NOT a pass/fail signal for "the chain damages near-end voice." Conversely ST_NE has no far-end echo, so `aecmos_echo` / `aecmos_dt` have nothing to score.

Floor enforcement applies only where ✓ in this table:

| Metric | `doubletalk` | `nearend_singletalk` | `farend_singletalk` |
|---|:-:|:-:|:-:|
| `erle_true_median_db` | ✓ (when oracle present) | n/a | ✓ (when oracle present) |
| `dnsmos_sig`  | ✓ | ✓ | — (informational only) |
| `dnsmos_bak`  | ✓ | ✓ | ✓ |
| `dnsmos_ovrl` | ✓ | ✓ | — (informational only) |
| `aecmos_echo` | ✓ | n/a | ✓ |
| `aecmos_other`| (informational) | (informational) | (informational) |
| `aecmos_dt`   | ✓ | n/a | n/a |

The CORPUS VERDICT in `run_aec_challenge.py` should respect this matrix (follow-up code change). Until that lands, the gate over-blocks on ST_FE's `dnsmos_sig`/`dnsmos_ovrl`; the v2 grade doc footnotes which BLOCKs are "structural" vs "real."

### 4. How the bar is applied (workflow)

```
ecnr_bench / ecnr_live  →  WAV per condition
                              ↓
ecnr_eval --run         →  results.csv  (ERLE columns)
                              ↓
reference/score_mos.py  →  results_with_mos.csv  (+ MOS columns)
                              ↓
reference/check_acceptance_bar.py  (new, lands with this ADR)
                              ↓
exit code 0 = green; non-zero = which metric / which condition failed
```

`reference/check_acceptance_bar.py` is a thin gate (one screen of Python) that reads `results_with_mos.csv`, compares each row against §2's targets + floors, and emits a per-condition pass/fail report. Returns non-zero on any floor miss; warns on any soft-target miss without failing.

The gate is informational by default. Once the chain consistently passes on the Phase-1 condition set, the gate can be wired into CI to catch regressions.

### 5. Re-grading cadence

The bar **must** be re-derived against measured data when:
- The Phase-2 cabin corpus exists. The current numbers are best-guess against literature + small-sample synthetic conditions; the 134-case corpus's actual distribution will require sharpening.
- The AECMOS inference path is implemented (today AECMOS columns are NaN; the `aecmos_*` targets are aspirational pending model wiring).
- A specific 3GPP TS 26.131 lab booking is scheduled — at that point the internal bar should be CONSERVATIVE relative to the lab spec, with at least 3 dB / 0.3 MOS of headroom across all targets to absorb test-setup variance.

A re-derivation lands as ADR-0013, supersedes this one.

## Trade-off analysis

What the bar **buys**:
- A defensible internal pre-cert quality target that's measurable today with no special hardware. Replaces "it sounds OK" with six per-condition numbers.
- A binary green/red signal on whether to spend the next quarter on tuning vs lab booking.
- A regression-catcher for the chain (CI hookable). Currently we have ctest at the unit level + listening A/B at the perceptual level, and a gap between them; this fills it.

What the bar **costs**:
- DNSMOS + AECMOS model downloads (~50 MB total once AECMOS is wired). Already gated by `score_mos.py`'s `--*-model` flags so a developer without models can still run the unit tests.
- The numbers in §2 are explicitly "best informed guess against literature." Until Phase 2 data lands, they may be too tight (false-fail on real-world recordings that real users would judge fine) or too loose (false-pass that lab booking would catch). The re-grading cadence in §5 is the safety valve.
- Six metrics × 4 conditions = 24 cells in the gate. Each new whitelisted EchoCanceller3Config sweep value (per ADR-0011) multiplies that. Gate runtime stays small (DNSMOS is ~1 s per condition on CPU; AECMOS comparable) but the report size grows.

## Consequences

**What becomes easier:**
- Tuning loops have a target. "Is this version better?" → run the gate, look at the deltas.
- ADR-0011's open assumption A2 (near-end damage metric) is closed by §1 — `dnsmos_sig` is the metric.
- The 3GPP TS 26.131 cert path has a defensible "go" criterion. Booking lab time without passing the internal bar is now an explicit overrule.
- A future-direction signal: a Phase-1-pass that still misses `dnsmos_bak` is a clear "RNNoise tuning" task; a Phase-1-pass that misses `aecmos_dt` is a clear "doubletalk handling — AEC3 tuning or neural RES" task.

**What becomes harder:**
- The bar is OPINION-DENSE; the §2 numbers reflect a specific reading of DNSMOS / AECMOS literature + project context. Other readings would justify different floors. Mitigated by the §5 re-grading cadence + ADR superseding pattern.
- Phase 1 closeout now depends on having `score_mos.py` run with at least DNSMOS available. AECMOS not wired means three of the six metrics are NaN — the gate still works, but on three columns only. Document in the closeout note.

**What we'll need to revisit:**
- §2 numeric targets after Phase 2 data — the dominant re-derivation trigger.
- §3 condition set when Phase 2 corpus is in hand.
- §1 panel composition if the AECMOS port reveals semantic overlap we hadn't anticipated.

## Open assumptions

- **A1: DNSMOS P.835 SIG correlates well with the near-end damage we care about.** Microsoft Research validates DNSMOS-SIG against ITU-T P.808 crowdsourced MOS, but their validation set is mostly call-centre conditions; in-cabin road-noise is under-represented. If a Phase-2-grade listening study reveals SIG ≠ in-cabin-perceived-quality, we'd need PESQ or POLQA as an alternate.
  - **v2 update (2026-05-31)**: PARTLY CLOSED. AEC-Challenge real-recording corpus gives `dnsmos_sig` p50 = 3.46 on ST_NE clips (clean near-end voice), which matches the literature's "good quality preserved" band. DNSMOS-SIG is behaving sensibly. The in-cabin question is still open until Phase-2 recordings — A1's premise is right; its scope narrows to "in-cabin specifically."
- **A2: AECMOS doubletalk score is a good proxy for the artifact the user is hitting today.** The user's observation is voice-over-suppression on non-stationary noise (cafe babble), which is more an NS problem than an AEC doubletalk problem. `aecmos_dt` may not move much; `dnsmos_sig` is the more direct measurement. We grade both and see.
  - **v2 update**: CLOSED. AEC-Challenge DT p50 = 3.28 across AGC on/off; both perceptual oracles agree on near-end damage when present. `aecmos_dt` does move (3.28 on real recordings vs ~2.90 on the over-suppressed synthetic fixtures), so it's informative. Keep both metrics in the panel.
- **A3: 4 conditions are enough for Phase-1 closeout grading.** Statistically thin — but the gate is per-condition, not aggregated, so a 4-of-4 pass is informative even at small N. Phase 2 fixes this.
  - **v2 update**: SUPERSEDED. The 30-clip AEC-Challenge subset replaces the 4-condition set as the primary corpus (§3). 30 clips × 3 scenarios gives proper per-scenario percentiles (p10/p50/p90) instead of single-point reads.
- **A4: The targets in §2 are aspirational but achievable.** Without measurement on the current chain we don't know — they're informed by DNSMOS literature thresholds + AECMOS's "typical good chain" reference points. The first run of the gate against the current chain will calibrate this; if everything misses by a lot, the bar is wrong, not the chain.
  - **v2 update**: PARTLY CLOSED. Recalibrated numbers in §2.1 are anchored on measured baselines, not literature. **Remaining aspirational**: the `dnsmos_sig` floor of 3.0 on DT is not yet cleared without AGC; we hold the floor at 3.0 because AGC-on does clear it (3.02) — but a small DT regression could flip it. Re-check after every chain change.
- **A5 (new, v2)**: NS-blend tuning cannot move the gate on real-recording content. Two corpus sweeps (synthetic and AEC-Challenge) both show `rnnoise_default` is at or near optimum; Step A/B blends are net-negative or marginal on every applicable metric. Locks in `rnnoise_default` as the production default. The ST_FE residual-leakage floor is structural and motivates Phase 3 RES.
- **A6 (new, v2)**: AGC default policy: AGC-on flips doubletalk from BLOCK to PASS on `dnsmos_sig`/`dnsmos_ovrl` (3.02/2.71 vs 2.98/2.68). ST_NE marginally hurt but still passes; ST_FE marginally hurt but blocked structurally regardless. **v0.4 should default `--agc` on.** AGC `max_gain_db` tuning has no signal on this corpus (cap never engaged); keep WebRTC default of 50. Re-check on cabin recordings where the cap may matter.

## Action items

**Completed (v1):**
- [x] **Phase 1, week 1:** implement `reference/check_acceptance_bar.py` per §4. Single Python file, ~50 LOC, no external deps beyond stdlib.
- [x] **Phase 1, week 1:** download DNSMOS P.835 ONNX into `models/dnsmos_p835.onnx` (~10 MB). Add `models/` to `.gitignore` if not already there.
- [x] **Phase 1, week 1:** run the full gate on the current chain against §3's 4 conditions. Publish results as `docs/phase-1-acceptance-grade.md`.
- [x] **Phase 1.5 (AECMOS wiring):** AECMOS scoring landed; all six metrics populated.

**Completed (v2):**
- [x] **AEC-Challenge integration (2026-05-31):** 30-clip subset + `run_aec_challenge.py` + first-run grade [`phase-1-acceptance-grade-aec-challenge.md`](../phase-1-acceptance-grade-aec-challenge.md).
- [x] **NS-blend corpus sweep (2026-05-31):** 9 configs × 30 clips via `sweep_ns_blend.py --config-set ns --manifest`.
- [x] **AGC max_gain_db corpus sweep (2026-05-31):** 7 configs × 30 clips via `sweep_ns_blend.py --config-set agc --manifest`. Plumbed `--agc-max-gain-db` CLI flag through to APM config.
- [x] **§2.1 + §3.1 recalibration:** measured-baseline-informed floors / targets + per-scenario applicability matrix locked.

**Open (next sprint):**
- [ ] Update `run_aec_challenge.py` CORPUS VERDICT logic to respect §3.1 applicability matrix (skip `dnsmos_sig`/`dnsmos_ovrl` on ST_FE; skip `aecmos_echo`/`aecmos_dt` on ST_NE).
- [ ] Update `run_aec_challenge.py` floor/target constants to the v2 §2.1 numbers.
- [ ] Flip the production default to `--agc on` per A6. Affects `ecnr_bench` default + any callers that build on top.
- [ ] Wire the gate into CI as a manual-trigger workflow on PRs touching `src/pipeline/`. Skip on PRs that only touch `docs/` / `reference/`.

**Phase 2 (gated on cabin recordings):**
- [ ] Re-grade against the cabin-recording corpus once it exists. Open ADR-0013 to supersede this ADR with cabin-grounded numbers.
- [ ] Re-sweep AGC `max_gain_db` on cabin content — current sweep's "cap never engages" result may not hold on quieter cabin SNR conditions.

## References

- [ADR-0001](0001-hybrid-aec-architecture-review.md) — the chain architecture this ADR grades.
- [ADR-0011](0011-aec3-tuning-methodology.md) — the eval harness `ecnr_eval` that produces the CSV this ADR keys off of.
- [`reference/score_mos.py`](../../reference/score_mos.py) — produces the MOS columns this ADR grades.
- [`docs/phase-2-cabin-recording-protocol.md`](../phase-2-cabin-recording-protocol.md) — captures the per-condition data the next-cut bar will be re-derived from.
- ITU-T P.835 — methodology DNSMOS estimates against.
- ITU-T P.808 — crowdsourced MOS, AECMOS's validation reference.
- 3GPP TS 26.131 / 26.132 — the *external* cert spec this internal bar serves as a stepping stone toward.

# ADR-0012: Phase-1 acceptance bar — internal numeric targets before 3GPP TS 26.131 cert

**Status:** Accepted (provisional — numbers locked against synthetic + a small set of recorded conditions; will sharpen once Phase 2 cabin recordings exist).
**Date:** 2026-05-19
**Resolves:** Open assumption A2 from [ADR-0011](0011-aec3-tuning-methodology.md) ("a near-end damage metric will be specified later") **plus** the broader question of "what numeric quality target are we trying to hit before booking 3GPP TS 26.131 lab time."
**Builds on:** [ADR-0011](0011-aec3-tuning-methodology.md) `ecnr_eval` harness; `reference/score_mos.py` (DNSMOS + AECMOS columns).

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

### 3. The condition set Phase 1 grades against

Phase 1 closeout grades against a **synthetic + small recorded** set, not the full 134-case Phase-2 corpus. Justification: Phase 2 isn't started, but we still need a defensible internal bar for v1.

| Set | Source | Count | Purpose |
|---|---|---|---|
| Synthetic (CI fixture) | `conditions/synthetic/case_001_quiet_cabin` per the ADR-0011 generator | 1 | sanity baseline; chain-mechanics regression catcher |
| Real-mic recorded — small | `reference/synth/test_mic_road.wav` + a clean-near-end clip | 2 | the listening-test fixtures already in the repo |
| Real-mic recorded — fixture | `reference/mixed_sound.wav` + matched ref | 1 | the user-supplied "near-end voice + heavy echo" fixture |

That's 4 conditions for Phase 1 grading. **All four must pass the per-metric floor.** Phase 2 expands to 6 (per the recording protocol) and eventually 24 (4-take × 6 condition matrix) and eventually 134 (the full corpus).

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
- **A2: AECMOS doubletalk score is a good proxy for the artifact the user is hitting today.** The user's observation is voice-over-suppression on non-stationary noise (cafe babble), which is more an NS problem than an AEC doubletalk problem. `aecmos_dt` may not move much; `dnsmos_sig` is the more direct measurement. We grade both and see.
- **A3: 4 conditions are enough for Phase-1 closeout grading.** Statistically thin — but the gate is per-condition, not aggregated, so a 4-of-4 pass is informative even at small N. Phase 2 fixes this.
- **A4: The targets in §2 are aspirational but achievable.** Without measurement on the current chain we don't know — they're informed by DNSMOS literature thresholds + AECMOS's "typical good chain" reference points. The first run of the gate against the current chain will calibrate this; if everything misses by a lot, the bar is wrong, not the chain.

## Action items

- [ ] **Phase 1, week 1:** implement `reference/check_acceptance_bar.py` per §4. Single Python file, ~50 LOC, no external deps beyond stdlib.
- [ ] **Phase 1, week 1:** download DNSMOS P.835 ONNX into `models/dnsmos_p835.onnx` (~10 MB). Add `models/` to `.gitignore` if not already there.
- [ ] **Phase 1, week 1:** run the full gate on the current chain against §3's 4 conditions. Publish results as `docs/phase-1-acceptance-grade.md`. Use the actual numbers to validate / adjust §2.
- [ ] **Phase 1, week 2:** wire `check_acceptance_bar.py` into CI (or a documented manual step in the release checklist).
- [ ] **Phase 1.5 (gated on AECMOS wiring):** re-run the gate with the AECMOS columns populated. Adjust `aecmos_echo` / `aecmos_dt` floors if literature numbers diverge from the chain's actual performance.
- [ ] **Phase 2 (gated on cabin recordings):** re-grade against the 134-case corpus. Open ADR-0013 to supersede this ADR with measurement-grounded numbers.

## References

- [ADR-0001](0001-hybrid-aec-architecture-review.md) — the chain architecture this ADR grades.
- [ADR-0011](0011-aec3-tuning-methodology.md) — the eval harness `ecnr_eval` that produces the CSV this ADR keys off of.
- [`reference/score_mos.py`](../../reference/score_mos.py) — produces the MOS columns this ADR grades.
- [`docs/phase-2-cabin-recording-protocol.md`](../phase-2-cabin-recording-protocol.md) — captures the per-condition data the next-cut bar will be re-derived from.
- ITU-T P.835 — methodology DNSMOS estimates against.
- ITU-T P.808 — crowdsourced MOS, AECMOS's validation reference.
- 3GPP TS 26.131 / 26.132 — the *external* cert spec this internal bar serves as a stepping stone toward.

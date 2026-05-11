# ADR-0011: AEC3 tuning methodology + ERLE measurement harness

**Status:** Accepted (provisional — Phase 2 will exercise the harness against the planned 134-case corpus; harness contract is locked, per-condition tuning values are not in scope here)
**Date:** 2026-05-11
**Resolves:** PROJECT.md "Step C" deferred work ("AEC3 tuning to reduce NS input level — needs Phase 2 ground-truth ERLE numbers per condition"); makes Step C unblocked once Phase 2 recordings exist.
**Builds on:** [ADR-0006](0006-aec-chain-interface.md) (`AecChain` interface, including the `ChainStats::echo_return_loss_enhancement_db` field already surfaced from AEC3); [ADR-0009](0009-media-aware-aec.md) (mode controller, which any future per-condition config switching will compose with).
**Out of scope:** specific AEC3 config values for specific conditions — those are tuning, not architecture, and live in a per-condition config file produced from this methodology, not in this ADR.

## Context

AEC3 exposes a large config surface (`webrtc::EchoCanceller3Config`, ~100 fields) and behaves materially differently across cabin conditions: HVAC-on vs HVAC-off, near-end voice active vs silent, music vs speech far-end, render level swings of ±15 dB. The chain currently runs at WebRTC defaults end-to-end. Phase 0.5 chose those defaults explicitly and PROJECT.md flags this as **Step C** of the NS over-suppression mitigation: AEC3 leaves more residual echo than it should under specific conditions, which then becomes input to RNNoise, which is what triggers the chopped-voice artifact already mitigated at NS by Steps A and B.

Step C wants to reduce that residual at the AEC3 layer instead of compensating at the NS layer. But "reduce residual" is a measurement question before it's a tuning question, and the chain currently has no honest ERLE oracle — `ChainStats::echo_return_loss_enhancement_db` is AEC3's *self-report*, not an external measurement. We can't tune against a number AEC3 is itself producing without risking optimising AEC3's confidence rather than its actual cancellation quality.

This ADR locks the **methodology**: how we measure, what we sweep over, where results live, what binary runs it. It deliberately does not pick numbers — those come from running the harness against real recordings.

## Decision

### 1. ERLE measurement: AEC3-reported AND true ERLE, both emitted per run.

The harness emits **two** ERLE numbers per (condition, config) cell:

- **`erle_reported_db`** — `ChainStats::echo_return_loss_enhancement_db` straight out of AEC3 (median over the run, after a settle window). Already exposed by ADR-0006. Operational telemetry; what production runtime reports.
- **`erle_true_db`** — externally computed as `10 · log10(P_echo_in / P_echo_residual)`, where:
  - `P_echo_in` = power of the echo signal at the mic, with no near-end voice and no noise. Comes from the per-condition `echo_only_mic.wav` fixture (see §3).
  - `P_echo_residual` = power of `AecChain::ProcessCapture(echo_only_mic, ref)` output, run on the same condition with the same config. The output of the chain when only echo is in the capture path is, by construction, the residual echo.

Both are RMS-windowed over the run with a configurable settle window (default 1 s) to skip AEC3 convergence.

Rationale:
- **Self-report bias** — `erle_reported_db` is what AEC3 thinks it's doing, not what it's doing. Tuning to it can produce configs that AEC3 *describes* well but that under-cancel in practice. The two-number contract makes the bias measurable: divergence between `erle_reported_db` and `erle_true_db` is itself a tuning signal.
- **True ERLE is cheap to compute** — one extra chain pass per (condition, config) cell on a `echo_only_mic` input. The chain has no internal randomness; running it twice on the same inputs is deterministic.
- **Decouples ERLE from near-end damage** — true ERLE only sees the echo-only path; near-end voice integrity is a *separate* metric (§4 open assumptions). This separation matters because raising ERLE often costs near-end intelligibility, and we want both axes visible.

Rejected alternative: estimating true ERLE from a single mixed-input run by spectral subtraction or stationary-noise estimation. Too many assumptions baked in; defeats the point of having an oracle.

### 2. Parameter sweep contract: explicit whitelist + sweep specification.

The harness reads a **sweep specification** (TOML) listing which `EchoCanceller3Config` fields are in the sweep, what values to try, and whether they vary independently or as a grid. Fields not on the whitelist are locked at WebRTC defaults — no exceptions, no implicit overrides.

Initial whitelist (Phase 2 entry, expandable as we learn):

| Field | Type | Why on the list |
|---|---|---|
| `echo_canceller3_mode` | enum (`kQuality`, `kRobust`, `kLowComplexity`) | Coarse first knob; sets aggressiveness baseline. |
| `EchoCanceller3Config::ep_strength.default_len` | float | Adaptive filter length; main lever on long reverb tails. |
| `EchoCanceller3Config::ep_strength.echo_can_saturate` | bool | Behaviour under loudspeaker saturation; cabin-relevant at high render. |
| `EchoCanceller3Config::suppressor.normal_tuning.mask_lf.enr_transparent` | float | Low-freq suppressor floor; closest knob to the over-suppression artifact. |
| `EchoCanceller3Config::suppressor.normal_tuning.mask_hf.enr_transparent` | float | High-freq counterpart. |
| `EchoCanceller3Config::render_levels.poor_excitation_render_limit` | float | Render-tap gate; affects convergence under low render. |

Anything else is locked. Adding to the list is a separate, reviewable change — appending to the TOML schema and re-running. This prevents "let's tune everything" sprawl.

Sweep types supported:
- **`grid`** — Cartesian product of all whitelisted values. Use for ≤2 axes; explodes fast.
- **`one_at_a_time`** — vary one field while others hold default. Use for first-pass exploration; cheap.
- **`fixed`** — a single named config, no sweep. Use for validating against a candidate "production" config.

The TOML format is documented in `docs/eval/sweep-spec.md` (separate doc, lands with the harness implementation, not in this ADR).

Rejected alternative: programmatic config sweeps in C++. Putting the sweep dimension in source means a recompile per experiment. TOML decouples that.

### 3. Conditions schema: a directory of per-condition fixtures, manifest-pinned.

A **condition** is a directory containing:

```
conditions/<condition_id>/
  mic.wav            # mic capture: echo + near_end + noise mix (what the chain sees in production)
  ref.wav            # far-end render (the AEC reference signal)
  echo_only_mic.wav  # echo as received at the mic, no near_end, no noise (for true-ERLE)
  near_end_clean.wav # OPTIONAL: clean near-end voice (for near-end-damage metrics, §4 deferred)
  meta.toml          # sample_rate_hz, mic_geometry_id, scene, near_end_active, render_level_db, etc.
```

All five tracks are at the same sample rate (16 kHz or 48 kHz per ADR-0003) and the same length. `mic.wav` is _not_ necessarily `echo_only_mic + near_end + noise` — it can also be a real cabin recording, in which case `echo_only_mic` is captured separately (e.g., during a "silence the driver" pass with the same render-tap signal).

A top-level `conditions/MANIFEST.tsv` lists `(condition_id, sha256(mic.wav), sha256(echo_only_mic.wav), sha256(ref.wav), license)` and is checked into the repo. The actual WAVs are gitignored (the existing `*.wav` gitignore rule from PROJECT.md continues to apply) and fetched out of band — same pattern as `vendor/MANIFEST.tsv`.

Phase 2 will produce the real 134-case corpus. Phase 1.5 ships a 2- or 3-condition **synthetic fixture set** under `conditions/synthetic/` generated by an extension of `reference/gen_combined_demo.py` — sufficient for harness self-test and CI, not sufficient for actual tuning.

Rejected alternative: a single big multi-track WAV per condition. Convenient at first, but `sndfile` multi-track muddles channel semantics with track semantics, and we'd lose the ability to swap individual tracks (e.g., overlay a different near-end clip on the same echo path).

### 4. Results format: per-run CSV + JSON manifest.

Per `ecnr_eval` invocation:

- **`<out_dir>/results.csv`** — one row per `(condition_id, config_hash)` cell, columns:
  - `condition_id`, `config_hash`, `sample_rate_hz`
  - `erle_reported_db_median`, `erle_reported_db_p10`, `erle_reported_db_p90`
  - `erle_true_db_median`, `erle_true_db_p10`, `erle_true_db_p90`
  - `divergent_filter_fraction_max`, `delay_ms_median`, `frames_dropped`
  - `cpu_ms_per_frame_median` (so tuning regressions in CPU envelope are visible)
- **`<out_dir>/manifest.json`** — one record per invocation:
  - tool version (git rev), build flags, sweep spec hash, condition manifest hash, timestamp, hostname
- **`<out_dir>/configs/<config_hash>.toml`** — the resolved per-cell config (for reproducibility)
- **`<out_dir>/logs/<condition_id>_<config_hash>.log`** — chain-level logs for one cell (optional, behind a flag)

Hash inputs: `config_hash = sha256(serialized resolved EchoCanceller3Config)`, `condition_hash = sha256(mic.wav || echo_only_mic.wav || ref.wav)`. Stable across runs; deduplicates between runs.

CSV (rather than parquet/arrow) because the cell count is small (134 conditions × ≤50 configs ≈ 7K rows) and grep/awk-able results matter more than column compression.

Rejected alternative: a SQLite database. Pretty, but overkill. CSV+JSON travels through `git diff` and PR review without needing a tool.

### 5. Binary shape: a new `ecnr_eval` target, not a `--eval` flag on `ecnr_bench`.

A separate CMake target `ecnr_eval`, parallel to `ecnr_bench` and `ecnr_live`. Same library deps (`ecnr_pipeline`, `ecnr_hal`), different `main()`. Gated by `option(ECNR_BUILD_EVAL "Build the AEC3 tuning eval harness" ON)` matching the ADR-0010 / cross-build precedent for `ECNR_BUILD_LIVE`.

Rationale:
- **ecnr_bench is a single-condition processor; ecnr_eval is a sweep runner.** Conflating them complicates ecnr_bench's CLI (already at 6 flags) and forces the bench user to opt out of eval features they don't want.
- **Different testing surface** — bench tests are signal-quality smoke; eval tests are sweep-coverage and CSV-correctness.
- **Different deploy story** — bench may ship to the U300 for in-field smoke. Eval is a host-only tool, not deployed.

### 6. Harness self-test: 1 synthetic condition committed for CI.

A `conditions/synthetic/case_001_quiet_cabin/` is included in the test corpus (generated by `reference/gen_combined_demo.py --eval-case quiet_cabin`, lands with the harness implementation). The `ecnr_eval --self-test` mode runs the harness on this one condition with the default fixed config, asserts `erle_true_db_median > 10 dB` and `erle_reported_db_median > 8 dB` (loose bounds that catch chain regressions, not tuning regressions), and exits non-zero if either fails.

The synthetic conditions are checked into `conditions/synthetic/` (not gitignored — small fixtures, hand-generated, license-clean). The 134-case Phase 2 corpus is gitignored and fetched.

## Trade-off analysis

The chosen contract (true ERLE + reported ERLE + TOML sweep spec + CSV results + separate binary + synthetic CI fixture) is the **smallest viable surface** for a defensible tuning loop. Each rejected alternative was rejected for one of two reasons: it papers over a bias (e.g., single-input true-ERLE estimation), or it pushes complexity into a place where iteration is slow (e.g., source-code sweep specifications, SQLite results).

What the contract pays for explicitly:
- **+1 chain pass per (condition, config) cell** for the true-ERLE computation. Cheap (chain runs at RTF ~0.05 on host); a 134×50 cell matrix is ~6700 cells × 2 passes × ~1 s/pass ÷ RTF ≈ ~10 min wallclock end-to-end on a developer laptop.
- **+1 fixture file per condition** (`echo_only_mic.wav`). For real recordings this requires a "silence pass" during data capture — a real cost during Phase 2 recording. The Phase 2 capture plan needs to bake this in or this ADR's true-ERLE pillar collapses.

What it doesn't yet decide:
- **Near-end damage metric.** ERLE measures echo cancellation in isolation; it does not measure near-end voice degradation. The two are antagonistic: aggressive AEC3 settings raise ERLE but chew up near-end. A real tuning loop needs a near-end-damage metric too. The conditions schema reserves `near_end_clean.wav` for this; the metric (likely PESQ, STOI, or a custom in-band power ratio) is a follow-up decision — see open assumption A2.
- **What "good" looks like per condition.** The harness emits numbers; the *targets* against which we judge those numbers (per-condition ERLE floor, per-condition near-end ceiling) come from Phase 2 listener studies, not this ADR.

## Consequences

**What becomes easier:**
- Step C ("AEC3 tuning to reduce NS input level") goes from "blocked on data" to "blocked on data acquisition" — once Phase 2 recordings exist, the harness produces actionable per-condition numbers immediately.
- Any future ADR that wants to change AEC3 defaults (e.g., a media-mode-specific config under ADR-0009) has a measurement contract to justify against.
- Regression detection: a CI run of `ecnr_eval --self-test` catches chain regressions that don't manifest as test-suite failures (e.g., subtle AEC3 convergence breakage that still produces audio output).

**What becomes harder:**
- The Phase 2 capture plan now has a hard requirement: every recorded condition must include a corresponding `echo_only_mic.wav`. Failing to capture this for some conditions makes them unsuitable for tuning.
- The TOML sweep spec adds a configuration-language surface to learn. Mitigated by keeping the schema minimal (whitelist + values + sweep_type).
- Two ERLE numbers per cell require two clear column conventions in CSV outputs. Mitigated by the explicit `erle_reported_*` vs `erle_true_*` column prefixes — no developer should look at one and accidentally read the other.

**What we'll need to revisit:**
- The whitelist of swept `EchoCanceller3Config` fields. Initial list (§2) is informed but not validated against measurements. After the first Phase 2 sweep, fields with no measurable sensitivity should drop out and fields with surprise sensitivity should be added.
- The near-end damage metric (open assumption A2).
- The settle-window default (1 s) — AEC3 convergence varies with the sweep parameters; we may need a per-config settle window estimator.

## Open assumptions

- **A1: True ERLE is operationally meaningful in cabin conditions.** True ERLE assumes a clean `echo_only_mic.wav` is capturable. In a real moving vehicle, traffic noise and HVAC bleed into the "silent pass". Worst case the noise floor in `echo_only_mic.wav` makes `erle_true_db` artificially low. Mitigation: Phase 2 capture protocol should record `echo_only_mic` in cabin-silent moments (engine off, HVAC off, windows up) when feasible; for road-noise conditions, document the noise-floor offset in `meta.toml`.
- **A2: A near-end damage metric will be specified later.** ERLE alone is not enough; aggressive tuning will sacrifice near-end voice quality. Candidates: PESQ (ITU-T P.862, reference-required, well-known), STOI (intelligibility, open-source), or a custom in-band-power-ratio metric. Decision deferred to ADR-0012 once we have near-end-clean recordings to validate metric stability against.
- **A3: AEC3's reported ERLE and true ERLE converge under healthy conditions.** Untested. We expect the two to track within ±2 dB when AEC3 is well-tuned and to diverge under pathological configs. If the relationship is wilder than that — e.g., reported is always 3-5 dB lower than true, or vice versa — that's worth its own follow-up note in REPORT-style format.
- **A4: 6 whitelisted fields is enough for the first useful tuning pass.** Informed by the WebRTC AEC3 docs and the project's prior chopped-voice failure mode (suggesting suppressor-side tuning is the main lever). Validation: if the Phase 2 sweep finds no significant ERLE delta across these 6 fields, the whitelist is wrong, not the fields' tuning.
- **A5: Chain determinism holds across re-runs.** Required for `config_hash` and `condition_hash` to be stable. RNNoise and AEC3 both have internal state but no PRNG; the chain should be bit-stable on identical inputs. The harness's self-test asserts this on case_001.

## Phase 2 entry criteria for using this harness in anger

This ADR's recommendations actually translate into tuning when **all** of the following are true:

1. The harness binary `ecnr_eval` is implemented and `--self-test` passes against the synthetic fixture.
2. Phase 2 cabin recordings (the 134-case corpus per `docs/phase-2-cabin-characterization-plan.md`) include matched `echo_only_mic.wav` for each case.
3. ADR-0012 (near-end damage metric) is accepted or explicitly deferred with a fallback.
4. A `docs/eval/sweep-spec.md` documenting the TOML schema and at least one example sweep is checked in.

## Action items

- [ ] **Phase 1.5, week 1:** add `src/eval/` directory with `eval_main.cc`, `metrics.cc`, `metrics.h`, `sweep_spec.cc`, `sweep_spec.h`.
- [ ] **Phase 1.5, week 1:** implement `ecnr_eval --self-test` against a single synthetic condition (`conditions/synthetic/case_001_quiet_cabin/`). This is the harness's hello-world.
- [ ] **Phase 1.5, week 1:** wire `ECNR_BUILD_EVAL` CMake option (default ON; matches ADR-0010 / cross-build pattern). Cross-build flips OFF until a Phase 2 sweep needs to run on the target (unlikely; this is a host-side tool).
- [ ] **Phase 1.5, week 2:** extend `reference/gen_combined_demo.py` (or a sibling script) to emit the 4-track condition layout (`mic.wav`, `echo_only_mic.wav`, `ref.wav`, optional `near_end_clean.wav`).
- [ ] **Phase 1.5, week 2:** unit tests for `metrics.cc` — true-ERLE math on synthetic sinusoid signals with known echo paths.
- [ ] **Phase 1.5, week 2:** write `docs/eval/sweep-spec.md` documenting the TOML format and the 3 sweep types (`grid` / `one_at_a_time` / `fixed`).
- [ ] **Phase 1.5, week 3:** add README CLI reference for `ecnr_eval`.
- [ ] **Phase 1.5 closeout:** run `ecnr_eval` against the synthetic fixture set with a `one_at_a_time` sweep over the 6 whitelisted fields. Confirm sweep mechanics; do NOT publish the resulting tuning values as production — synthetic fixtures are not representative.
- [ ] **Phase 2 (gated on cabin recordings):** open ADR-0012 — near-end damage metric.
- [ ] **Phase 2 (gated on cabin recordings):** run `ecnr_eval` on the 134-case corpus. Outputs feed Step C tuning decisions.

## References

- [ADR-0001](0001-hybrid-aec-architecture-review.md) — open assumption Q3 (parameter tuning vs PESQ/STOI) is the upstream of this ADR.
- [ADR-0006](0006-aec-chain-interface.md) — `ChainStats::echo_return_loss_enhancement_db`; this ADR consumes that field as `erle_reported_db` and adds `erle_true_db` alongside.
- [ADR-0009](0009-media-aware-aec.md) — mode controller; any per-mode config switching composes with this methodology (the sweep would just include `mode` as one of its axes).
- [`docs/phase-2-cabin-characterization-plan.md`](../phase-2-cabin-characterization-plan.md) — 134-case corpus; this ADR adds a per-case `echo_only_mic.wav` requirement.
- [`PROJECT.md`](../../PROJECT.md) "Known limitations (deferred work)" — Step C; this ADR is the methodology that unblocks Step C tuning.
- [`src/pipeline/aec_chain.h`](../../src/pipeline/aec_chain.h) — `ChainStats` struct; consumed as-is.
- WebRTC `EchoCanceller3Config` reference: `vendor/webrtc-audio-processing/webrtc/modules/audio_processing/aec3/echo_canceller3_config.h`.

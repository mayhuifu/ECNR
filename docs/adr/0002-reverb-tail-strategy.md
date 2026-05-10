# ADR-0002: Reverb-tail strategy — cabin IR characterization & AEC3 tail config

**Status:** Open (stub) — gated on Phase 2 cabin acoustic measurements
**Date:** 2026-05-10
**Supersedes assumption A1 in:** [ADR-0001](0001-hybrid-aec-architecture-review.md)

## Why this ADR is a stub

ADR-0001's open assumption A1 flagged that **WebRTC AEC3's filter tail length** — designed for handsets / web conferencing where reverb is ~50–150 ms — may be insufficient for car cabins, which typically run **150–300 ms** RT60 (and longer with rear-passenger mics, windows down, soft-surface variation, music-system speakers in unusual placements).

Phase 0.5 shipped AEC3 with **default config** (`webrtc::EchoCanceller3Config{}`) which targets ~250 ms of effective filter coverage at 16 kHz / ~150 ms at 48 kHz on a typical handset acoustic path. Whether that's sufficient for the U300 cabin is **unmeasured** — the project does not yet have:

1. Cabin impulse-response measurements (driver mic ← head-unit speakers).
2. Per-vehicle / per-trim-level RT60 estimates.
3. Empirical ERLE drop-off vs configured tail length on cabin material.

Until Phase 2 produces those measurements, any concrete tail-length decision is speculation. This ADR exists to formally **register the question** so it isn't lost between phases, and to enumerate the decision options that will actually be considered when the data arrives.

## Decision (deferred to Phase 2)

When cabin IR measurements are available, this ADR will choose between:

- **Option A — Keep AEC3 default config.** Acceptable if measured ERLE on real cabin material is ≥ 15 dB at typical SNR (default tail covers the cabin's energy-significant reverb).
- **Option B — Tune `EchoCanceller3Config::filter`** (longer tail). Acceptable if default is insufficient AND AEC3 supports the required tail length (it has a configurable filter length up to ~500 ms; longer needs custom build).
- **Option C — Frequency-domain block AEC with explicit longer-tail support.** Switch to SpeexDSP AEC, Athena-signal, or a custom freq-domain implementation. Bigger code change; reserved for if AEC3 cannot reach the cabin tail.
- **Option D — Two-stage AEC.** AEC3 for the first ~150 ms + a long-tap residual canceller for the remaining 150–300 ms. Higher CPU; potentially more robust on long, sparse reverb.

The chosen option will depend on the **gap between AEC3-default and target ERLE**, not on a-priori preference.

## Open questions (will be answered by Phase 2 data)

1. **What is RT60 in the U300 target vehicle(s)?** Typical sedan: 80–120 ms. Typical SUV with rear seats: 150–250 ms. Convertible / hardtop down: > 300 ms. Range matters because it sets which option is viable.

2. **Is reverb correlated with driving conditions?** Windows-down highway driving has different acoustics from windows-up urban. The chain's tail config may need to be a tier (idle / driving / windows-down) rather than a single value — coupling this ADR to ADR-0009 (mode controller, render-type) and Phase 4.

3. **Is multi-mic placement an answer?** Driver-mic-only AEC sees the worst reverb path. A driver-pillar mic + rear-zone mic with proper beamforming (ADR-0010 → real Beamformer in Phase 1) may reduce the AEC's effective tail-length requirement by selecting a closer mic to the source.

4. **What is the AEC3 ERLE roll-off vs tail length?** A measurement plan: feed a swept stimulus (white noise) through the head-unit, capture at the driver mic, run AEC3 at default + tuned tail lengths, plot ERLE vs configured filter length. Determines whether Option B has measurable upside.

## Phase 2 entry criteria

This ADR resumes when:

- **A1**: a vehicle is available for cabin IR measurement (any U300 trim level).
- **A2**: a calibrated mic / known stimulus / room treatment baseline has been chosen.
- **A3**: the Phase 2 cabin corpus exists (per [PROJECT.md](../../PROJECT.md) roadmap row).

Until then: AEC3 default config is in production, and the test threshold (`AttenuatesCorrelatedEcho` > 15 dB) is the only gate. Real cabin material will likely lower observed ERLE below this synthetic-stimulus number; that's expected and is precisely what Phase 2 measures.

## Forward-looking links

- ADR-0010 (real beamformer) reduces the burden on AEC3 by improving input SNR — a longer cabin tail is more tolerable when the post-beamform signal already has 6–9 dB of directional gain.
- ADR-0008 trigger T1 (RTF) and T2 (energy/sec) are tail-sensitive: a longer filter is more CPU. If Option B raises RTF above 0.4, Phase 6 (DSP offload) becomes more interesting.
- ADR-0009 (media-aware AEC): if music as render becomes load-bearing, the tail decision may differ for music vs voice (music's longer correlation time vs voice's faster transients).

## Action items

- [ ] **Phase 2 — Cabin IR measurement plan.** Document the measurement protocol (mic, stimulus, vehicle access, calibration). Owner: TBD when Phase 2 starts.
- [ ] **Phase 2 — AEC3 ERLE-vs-tail-length sweep.** On real cabin material, plot ERLE at default / +50% / +100% / +200% configured tail. Decides Option A vs B vs C/D.
- [ ] **Cross-check with ADR-0010 deliverable.** Once a real beamformer is integrated, re-measure cabin ERLE — beamforming reduces the required tail.

## References

- [ADR-0001 §A1](0001-hybrid-aec-architecture-review.md) — original assumption.
- [`vendor/webrtc-audio-processing/webrtc/api/audio/echo_canceller3_config.h`](../../vendor/webrtc-audio-processing/webrtc/api/audio/echo_canceller3_config.h) — APM-side knobs (after `scripts/fetch-vendor.sh required`). The `filter` substruct is the primary tuning surface.
- [ADR-0008](0008-dsp-offload-criteria.md) — trigger T1/T2 are tail-sensitive.
- [ADR-0010](0010-mic-geometry-and-beamforming.md) — beamforming reduces tail burden.

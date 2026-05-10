# Phase 2 — Cabin Acoustic Characterization Plan

> **Status:** Draft (2026-05-10). Triggers when vehicle access is available. This doc captures the protocol, deliverables, and gating decisions so the work can start cold without re-deriving anything.

## Why Phase 2 exists

Phase 0.5 shipped a real AEC3 + RNNoise chain, validated on a synthetic stimulus (white noise echoed through a Mac speaker into a Mac mic) and a successful end-to-end listening test (speech preserved, echo cancelled). That's a *minimum-viable host validation*. It does **not** prove the chain works in a U300 cabin.

Several open ADRs depend on real cabin measurements before they can be resolved:

- **[ADR-0002](adr/0002-reverb-tail-strategy.md)** — Reverb tail strategy. Pure stub today. Decides between AEC3 default config / tuned tail / freq-domain block AEC / two-stage AEC based on measured RT60.
- **[ADR-0009](adr/0009-media-aware-aec.md)** — Media-aware AEC (`RenderType` hint policy). Open assumption: the actual quality regression on music under voice-trained NS is unmeasured. PESQ/SDR sweep needed.
- **[ADR-0010](adr/0010-mic-geometry-and-beamforming.md)** — Real beamforming. Decides whether DSB is sufficient or MVDR/GSC needs to land. Open assumptions about U300 mic positions + cabin SNR target.
- **[ADR-0008](adr/0008-dsp-offload-criteria.md)** — Trigger T1 (RTF) and T2 (energy/sec) need real-cabin baselines, not synthetic-stimulus numbers, to fire confidently.

Phase 2 is the empirical work that unlocks A1/A2/A3-style ambiguity in the architecture review — for the cabin variant of the problem specifically.

## Hard prerequisites (block Phase 2 start)

- [ ] **A vehicle.** Any U300 trim level — the trim differences (sedan / SUV / convertible / soft-top) are themselves data. If only one vehicle is available, that's the floor; we'll need at least one more for cross-vehicle generalization later.
- [ ] **Stimulus playback path.** A way to play known reference signals through the head-unit at calibrated levels. Either: (a) USB / Bluetooth audio with the head-unit volume locked to a marked notch, or (b) direct line-in.
- [ ] **Capture path.** Either (a) the U300 mic array via the production HAL (cleanest — same path as production), or (b) a calibrated reference mic placed at the driver-mic position (lets us measure the cabin without depending on the HAL).
- [ ] **A baseline anechoic recording.** A short clip of the same stimulus + speech recorded in a quiet, low-reverb environment (a treated room, or worst-case a small closet) so we have a reference against which "what the cabin adds" can be quantified.

If item (b) for both paths is unavailable, Phase 2 can still proceed via the production HAL — it just means the HAL is in the loop and HAL bugs become indistinguishable from cabin acoustics. Easier to debug if the reference mic exists.

## Phase 2 deliverables

### Deliverable 1 — Cabin impulse responses (IRs)

For each vehicle × condition, capture the impulse response from each speaker to each mic in the cabin.

**Method:** ESS (exponential sine sweep) — emit a 5–10 s sweep from 20 Hz to 16 kHz (or 20 Hz to 24 kHz at 48 kHz tier), record at the mic, deconvolve to recover the IR. Standard tools: REW (Room EQ Wizard), pyacoustics, or our own `gen_synth.py` extension.

**Conditions to measure:**

| Condition | Engine | Speed | Windows | HVAC | Music |
|---|---|---|---|---|---|
| 1. Idle baseline | running | 0 mph | closed | off | off |
| 2. Idle + HVAC | running | 0 mph | closed | medium | off |
| 3. City driving | running | 30 mph (urban roll) | closed | low | off |
| 4. Highway | running | 65 mph | closed | low | off |
| 5. Highway + windows | running | 65 mph | half open | low | off |
| 6. Idle + music | running | 0 mph | closed | off | medium |

For each condition: per-speaker → per-mic IR + 30 s of ambient capture (no stimulus).

**Per-condition outputs:**
- `cabin_ir_<condition>_<speaker>_<mic>.wav` — measured IR.
- `cabin_ambient_<condition>.wav` — 30 s ambient capture, mics only.
- `cabin_metadata_<condition>.txt` — vehicle ID, mic positions, calibration notes.

**Decisions this enables:**
- ADR-0002 RT60 measurement → option A/B/C/D.
- ADR-0010 — does the cabin SNR vary enough across conditions to warrant adaptive beamforming, or does fixed DSB hold?

### Deliverable 2 — Reference corpus

A curated set of (input, expected-output) pairs for offline AEC + NS evaluation. Each test case has:

- **Render** (what the speaker plays): voice / TTS / music / silence.
- **Mic capture** (what the mic records): render-echoed-through-cabin + injected near-end speech + ambient noise.
- **Oracle near-end** (the expected ideal output of the chain): the injected near-end speech, isolated.

**Corpus sizes (Phase 2 first cut):**

| Subset | Cases | Duration | Purpose |
|---|---|---|---|
| Single-talk near-end | 30 | 10 s each | Driver speaks; near-end energy, speech intelligibility (PESQ) |
| Single-talk far-end | 30 | 10 s each | Speaker plays voice; AEC must reduce to silence |
| Double-talk | 30 | 10 s each | Driver speaks while speaker plays voice; ERLE + speech intelligibility under DTD |
| Music + speech | 20 | 10 s each | ADR-0009's load-bearing case |
| Conditions sweep | 6 × 4 = 24 | 10 s each | Across the 6 cabin conditions, with music+speech |
| **Total** | **134** | ~22 minutes | — |

**Process:**
1. Pre-record dry near-end speech (anechoic). Use TIMIT, LibriSpeech, or a local recording.
2. Pre-record render content: TIMIT (voice), commercial music samples, silence.
3. In each cabin condition, play render through speakers, near-end speaker injects speech via head-mounted mic (or close-talk lavalier) which is also recorded as the oracle.
4. Capture mic stream; pair with the render and oracle.

**Outputs:**
- `corpus/<subset>/<case>.tar.gz` per case, containing `render.wav`, `mic.wav`, `near_clean.wav`, `metadata.txt`.

### Deliverable 3 — Evaluation harness

Extend `ecnr_bench` to produce structured metrics, not just the current single-line summary. New flags:

- `--corpus <dir>` — point at the Phase 2 corpus directory; bench iterates all cases.
- `--metrics <out.json>` — emit per-case + aggregated metrics in JSON.

Metrics to compute:
- **ERLE** — already computed; per-case and aggregated.
- **PESQ** — speech-intelligibility metric for the chain's output vs the oracle. Needs a PESQ implementation; `python-pesq` exists for offline analysis.
- **STOI** — short-time objective intelligibility, complementary to PESQ.
- **SDR / SI-SDR** — signal-to-distortion ratio of the chain's output vs oracle. Standard NN-eval metric.
- **Frames dropped** — per-case `chain_dropped` count.

Aggregation:
- Mean / median / 5th / 95th percentile of each metric, per subset and overall.
- Pass/fail thresholds per metric (TBD — Phase 2 picks these from the data, not a-priori).

**This unlocks:**
- ADR-0002 — ERLE-vs-tail-length sweep on real material; pick option A/B/C/D.
- ADR-0009 — PESQ regression on music vs voice → quantifies the media-aware AEC value.
- ADR-0008 trigger T1 — RTF on real cabin material, not synthetic.

### Deliverable 4 — Updated ADRs

Phase 2 promotes [ADR-0002](adr/0002-reverb-tail-strategy.md) from stub to a concrete decision with the four options narrowed to one. Updates [ADR-0009](adr/0009-media-aware-aec.md) and [ADR-0010](adr/0010-mic-geometry-and-beamforming.md) with measured values for their open assumptions. Possibly opens **ADR-0011** (real beamforming algorithm choice) once the cabin SNR and noise-field characterization are in.

## Phase 2 entry checklist

When all four are checked, Phase 2 starts:

- [ ] Vehicle access secured (date + duration committed).
- [ ] Stimulus playback path verified (head-unit volume calibrated, render gain known).
- [ ] Capture path verified (HAL hookup OR reference mic placement decided).
- [ ] Anechoic baseline recording captured (reference for "what the cabin adds").

## Phase 2 → Phase 3 handoff

Phase 3 (neural RES integration per [ADR-0007](adr/0007-neural-runtime.md)) needs Phase 2's corpus to evaluate. Specifically:

- The 134-case corpus is the eval set against which TFLite-deployed NKF-AEC / DTLN-AEC / DeepFilterNet are scored.
- Phase 2's PESQ/STOI/SDR baselines (from AEC3 + RNNoise alone) are the "before" — Phase 3's neural RES has to beat them, or the model isn't earning its CPU.
- ADR-0007 O1 (INT8 PTQ accuracy on AEC residuals) becomes measurable: quantize the model, run on the Phase 2 corpus, compare PESQ/STOI deltas vs FP32.

## Open scoping questions for Phase 2

These need answers when planning starts (not now):

1. **Vehicle ownership / access timeline.** Daily / weekly / one-shot access? Affects whether we can do iterative measurements or have to nail it in one session.
2. **Calibrated reference mic budget.** A measurement-grade mic (Earthworks, Audix TM1) is a few hundred USD; a budget calibrated mic (Behringer ECM8000) is ~$70. Production HAL alone may be sufficient for first cut.
3. **Scope of "cabin conditions sweep".** The 6-condition table above is a first cut; expanding to e.g. multiple HVAC speeds × multiple road surfaces × multiple driver/passenger speech locations multiplies the corpus rapidly. Phase 2 first cut should be the 6 above; expansion is Phase 2.5+.
4. **Privacy / consent for speech recordings.** If the near-end speaker isn't the project lead, formal consent for use of their voice in test corpora is needed.
5. **Music licensing.** Commercial music samples for "music + speech" subset have licensing implications. Use royalty-free / public-domain / project-team-recorded music for the corpus.

## References

- [PROJECT.md](../PROJECT.md) — Phase 2 row in the roadmap.
- [ADR-0001](adr/0001-hybrid-aec-architecture-review.md) — open assumptions A1/A5 + automotive specifics that this phase resolves.
- [ADR-0002](adr/0002-reverb-tail-strategy.md) — primary consumer of the IR measurements.
- [ADR-0007](adr/0007-neural-runtime.md), [ADR-0008](adr/0008-dsp-offload-criteria.md), [ADR-0009](adr/0009-media-aware-aec.md), [ADR-0010](adr/0010-mic-geometry-and-beamforming.md) — secondary consumers of the corpus + measurements.
- ITU-T P.1100 / P.1110 — handsfree-in-vehicle acoustic test profile (industry standard for the test conditions).
- 3GPP TS 26.131 — speakerphone tests, related test methodology.

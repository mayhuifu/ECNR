# ADR-0001: Hybrid linear-AEC + neural-post architecture (review)

**Status:** Accepted (provisional — see open assumptions)
**Date:** 2026-05-09
**Deciders:** Project lead

## Context

The U300 system needs an in-cabin AEC + NR audio stack on **Cortex-A55**. Two consolidated research reports ([deep-research-report.md](../deep-research-report.md), [Cellular Audio Processing Solutions Deep Dive.md](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md)) converge on a hybrid recommendation: **classical linear AEC backbone + neural post-processing**, not pure end-to-end neural replacement. Phase 0/0.6 of the project is built; the architecture is currently encoded in [PROJECT.md](../../PROJECT.md) and the [`AecChain`](../../src/pipeline/aec_chain.h) interface.

This ADR retroactively formalizes the architecture and surfaces assumptions that have **not yet been validated** — the research is primarily *cellular* (VoLTE/VoNR), and translating it to *automotive in-cabin* introduces concerns the source material is thin on.

## Decision

Adopt the hybrid pipeline:

```
mic → resample → linear AEC (WebRTC AEC3) → neural RES → NS → AGC → uplink
       ▲                ▲                                   ▲
       │                │                                   │
   render-tap     delay estimator                  mode controller
```

with these top-level commitments:

1. **Linear AEC stays classical** (DSP-friendly, deterministic, cheap to verify).
2. **Neural lives in the post-filter only** (RES + NS), not the main echo cancellation.
3. **Cortex-A55 only for v1**; Tensilica HiFi DSP offload deferred.
4. **16 kHz / 10 ms frames** as the canonical chain rate.
5. **Mode controller** swaps algorithm depth based on activity (idle / NS-lite / AEC / AEC+NN).
6. **Reference signal tapped post-DRC/EQ/speaker-protection**, as close to the speaker driver as possible.

## Options considered

### Option A — Hybrid linear + neural post (chosen)

| Dimension | Assessment |
|---|---|
| Complexity | Medium |
| Cost (engineering) | Medium |
| Compute on A55 | Tractable (NKF-AEC: 5.3K params, RTF 0.09; AEC3 fits easily) |
| Power | Best of the three: fixed-cost linear core + small neural |
| Determinism / debuggability | High — linear AEC is well-understood; failures decompose |
| Path to DSP offload | Clean — linear AEC moves to DSP, neural stays on A55 |
| Risk profile | Low — every production deployment uses some variant of this |

**Pros:**
- Decouples *delay/clock/reference-routing* concerns (the actual production failure modes per the research) from *model* concerns. They fail differently and you can debug them separately.
- Multiple production-grade open-source baselines (WebRTC AEC3, Speex, RNNoise) — you can ship the baseline before any neural component lands.
- Neural component is contained and swappable (NKF-AEC ↔ DTLN-AEC ↔ Bark-scale RES).
- DSP offload story is natural: linear core to DSP, neural stays on ARM.

**Cons:**
- Two systems to tune, with a coupling boundary between them (residual-echo statistics fed into the post-filter).
- The "linear backbone tuned for handsets" assumption breaks for *long cabin reverb* — see open assumptions below.
- Neural post-filter inherits the linear AEC's blind spots (e.g., non-linear distortion the linear stage can't model).

### Option B — Pure end-to-end neural

| Dimension | Assessment |
|---|---|
| Complexity | Low (in code) / High (in operations) |
| Cost (engineering) | High — model training, dataset curation, on-device inference |
| Compute on A55 | Risky — large models (DPCRN, full DeepFilterNet) push thermal budgets |
| Power | Worst — every frame pays full inference cost |
| Determinism / debuggability | Low — black-box, hard to diagnose on-vehicle failures |
| Path to DSP offload | Awkward — DSPs prefer signal-processing graphs over generic NN |
| Risk profile | High — research consensus (CN report line 175) is "amplifies delay alignment, reference consistency, double-talk stability, and power issues simultaneously" |

**Pros:** Single concept; potentially better quality on non-linear distortion if the model is right.
**Cons:** All the production risks compound. Rejected by the source research and by automotive Tier-1 industry practice.

### Option C — Classical only (no neural)

| Dimension | Assessment |
|---|---|
| Complexity | Low |
| Cost (engineering) | Low |
| Compute on A55 | Trivial |
| Power | Lowest |
| Determinism | Highest |
| Quality on non-stationary noise (road, HVAC, music) | **Insufficient** — this is exactly where classical NS (Wiener, MMSE-LSA, RNN-free) struggles |

**Pros:** Easiest to ship. Most predictable. Best power.
**Cons:** Won't meet quality bar for cabin non-stationary noise, road noise at speed, or music-as-echo. Loses to competitive products that have neural post-processing. Acceptable as a *fallback* tier (and is in fact our "low-power tier"), not as the headline architecture.

## Trade-off analysis

The chosen hybrid is the **median** of the three on every axis: not the cheapest, not the highest quality, not the lowest risk — but the one with no failure mode that takes the project off the road.

The single most important reason to prefer Option A over the alternatives is **failure decomposition**. Production audio failures are dominated by:

1. Render-tap routing wrong → AEC has nothing to cancel against → looks like "AEC doesn't work"
2. Sample-rate / clock mismatch between mic and render → AEC adaptive filter diverges
3. Double-talk under-protected → near-end speech gets clipped
4. Reverb tail longer than filter → uncovered echo persists

A pure-neural pipeline turns all four into "the model output is bad" — opaque, hard to bisect. A hybrid lets the linear stage own (1)(2)(4) and the neural stage own non-stationary noise + non-linear residual. Each stage has metrics that say *what* is broken.

## Consequences

**What becomes easier:**
- Phase 0.5 productization — the baseline (AEC3 + RNNoise) is a known-quantity stack used by PulseAudio, PipeWire, WebRTC apps everywhere; lots of prior art for tuning.
- Cross-platform host testing — neural and classical both run on x86/ARM dev hosts identically.
- DSP offload story — linear AEC has decades of HiFi DSP precedent.
- Decomposable measurements — separate ERLE (linear stage) and SDR/PESQ (post-filter) attributable metrics.

**What becomes harder:**
- Two-stage tuning — boundary between linear residual and neural post needs explicit metrics; otherwise tuning either alone can degrade the other.
- License governance — neural models (DTLN-AEC, NKF-AEC) have less-clear license stories than the BSD/Apache classical stack; needs ongoing audit (see Phase 3 prerequisite).
- Model lifecycle — once we ship a TFLite/ONNX model, we need deployment, versioning, A/B, rollback infrastructure that didn't exist before.

**What we'll need to revisit:**
- All the open assumptions below — each is a candidate for its own ADR.

## Open assumptions (NOT yet validated)

These are the load-bearing assumptions in the architecture that are *plausible but unverified* for the automotive in-cabin variant of the problem. Each is a project risk and a candidate for its own ADR before/during Phase 0.5–2.

### A1. AEC3's tail length is sufficient for in-cabin reverb

**Assumption:** WebRTC AEC3's filter handles a long-enough tail for car cabins.
**Status:** **Unvalidated.** AEC3 was designed for handsets / web conferencing where reverb is ~50–150 ms. Car cabins typically run **150–300 ms** (and longer with rear-passenger mics + windows-down + soft-surface variation). AEC3's default config may need a custom tail-length (`EchoCanceller3Config::filter`).
**Validation:** Measure cabin IRs across vehicles; configure AEC3 tail to RT60 + safety margin; benchmark ERLE vs tail length sweep.
**Mitigation if wrong:** Switch to frequency-domain block AEC with explicit longer-tail support (Speex AEC, Athena-signal, custom).
**Recommended:** Promote to **ADR-0002 — Reverb tail strategy** before Phase 1.

### A2. 16 kHz is the right canonical rate for in-cabin

**Assumption:** 16 kHz is fine because the research says it's most efficient on A55.
**Status:** **Probably wrong for music.** The research is *cellular* — voice, no music. In a car, the speaker plays **music** as well as voice prompts/TTS. Music has spectral content above 8 kHz. AEC running at 16 kHz cannot cancel music components above 8 kHz; you'll hear the music leak into the uplink.
**Implications:** If music echo cancellation is a requirement, the canonical rate should be **32 kHz** (cuts off at 16 kHz, covers most musical brilliance) or **48 kHz** (full-band).
**Validation:** Define the use case explicitly: is the user expected to run hands-free *while playing media*? If yes, 32 kHz at minimum.
**Mitigation if wrong:** Bump to 32 kHz; AEC3 supports it; cost is ~2× CPU on A55. Frame loop stays 10 ms.
**Recommended:** Promote to **ADR-0003 — Canonical sample rate** before Phase 0.5 *(blocking — affects every interface)*.

### A3. Single-mic AEC is sufficient (multi-mic is "later")

**Assumption:** Phase 0–1 is single-mic; multi-mic / beamforming is a future phase.
**Status:** **Architecturally questionable for automotive.** Cabins are routinely **multi-mic** out of necessity (driver isolation, rear-passenger zones, voice-activity localization, beamforming for road-noise rejection). Single-mic AEC in a car has a far worse SNR ceiling than in a phone because:
- The acoustic path from speaker to mic is short and direct (high echo level).
- Background noise is non-stationary and high (45–65 dBA at highway speed).
- Wind/HVAC adds wideband non-stationary content that single-mic NS struggles with.
**Implications:** A single-mic AecChain interface (current `Frame` = mono int16) commits us to a re-architecture if multi-mic is needed.
**Validation:** Confirm with U300 product/HW: how many mics? geometry? per-zone or summed?
**Mitigation if wrong:** Generalize `Frame` to multi-channel **early** (Phase 0.5) — cheap now, expensive later. Add a "Beamformer" stage **before** AEC in the chain.
**Recommended:** Promote to **ADR-0004 — Mic geometry & beamforming** before Phase 0.5 *(blocking)*.

### A4. The render tap is accessible at the right point

**Assumption:** The U300 audio HAL exposes a render tap *post-DRC/EQ/speaker-protect*.
**Status:** **Unknown.** Many automotive audio HALs (SoC vendor stacks: Qualcomm SnapdragonAuto, NXP, Renesas) do **not** expose this; the speaker-protection / DRC / EQ chain runs on a closed DSP and the tap point available to host software is *pre-* those stages. Mismatch between AEC reference and actual driver signal is the #1 production failure mode (see CN report risks list).
**Validation:** HAL audit with U300 platform team.
**Mitigation if wrong:** Either (a) measure DRC/EQ/SP transfer functions and compensate with a fixed pre-emphasis on the render tap, or (b) require HAL change to expose the right tap, or (c) accept a residual-echo cap and lean harder on the neural post-filter.
**Recommended:** Promote to **ADR-0005 — Render tap policy** ASAP. *Highest-impact unknown in the project.*

### A5. Music-as-echo will be handled by the same AEC

**Assumption:** AEC + post-filter cancels both voice prompts AND playing music.
**Status:** **Probably needs a separate path.** Music has very different statistics from voice (long decay, full-band, high crest factor). Linear AEC is fine but the *neural post-filter* trained on voice residuals may treat music as "noise to suppress" or "echo to attenuate" inconsistently. Some automotive stacks use a **media-aware AEC** that knows when music is playing and adjusts.
**Validation:** Test the chain with music playback as the render signal; measure if the post-filter introduces musical artifacts.
**Mitigation if wrong:** Add a "render type" hint into the chain (voice vs music), with the post-filter selecting different behavior or being bypassed for media playback.
**Recommended:** Promote to **ADR-0009 — Media-aware AEC** before Phase 3 (neural integration). *(Originally reserved as ADR-0006; renumbered when 0006 was assigned to the AecChain interface ADR on 2026-05-10.)*

### A6. The `AecChain` interface is API-compatible with WebRTC's `AudioProcessing`

**Assumption:** The current Phase 0 `AecChain::ProcessRender(Frame)` / `ProcessCapture(Frame, out)` shape will accept `webrtc::AudioProcessing::ProcessReverseStream` / `ProcessStream` underneath without re-design.
**Status:** **Probably yes, but unaudited.** WebRTC's APM has additional hooks the stub ignores: `set_stream_delay_ms()`, `set_stream_analog_level()`, level estimators, output stats (residual-echo-likelihood, etc.). If our HAL provides delay info or AGC signaling, we should propagate it.
**Validation:** 30-minute audit: read [`webrtc/modules/audio_processing/include/audio_processing.h`](../../vendor/webrtc-audio-processing/webrtc/modules/audio_processing/include/audio_processing.h) and list every method our `AecChain` should plausibly expose.
**Mitigation if wrong:** Expand `AecChain` interface in Phase 0.5 *before* commit. Cheap now.
**Recommended:** Action item below; doesn't need its own ADR.

### A7. `ExternalProject_Add` will scale to A55 cross-compile

**Assumption:** Building WebRTC (Meson) and RNNoise (autotools) under CMake `ExternalProject_Add` will work for both host and A55 cross-compile.
**Status:** **Likely fragile.** Meson + cross-compile + ExternalProject is a known sharp edge. autotools + cross-compile + ExternalProject ditto. The orchestration that works on macOS host may not work cross-targeting `aarch64-linux-gnu`.
**Validation:** Phase 0.5 prototype builds on macOS first; Phase 1 cross-compile is its own integration spike.
**Mitigation if wrong:** Either (a) write CMakeLists for the specific WebRTC and RNNoise files we use (curated subset), or (b) build them out-of-band with their native build systems and `find_library` the result, or (c) bind to system packages on A55 target.
**Recommended:** Plan a **Phase 1 cross-compile spike** as a discrete task; not blocking Phase 0.5.

## Action items

The following ADRs should be opened (or merged into Phase 0.5/1 design):

1. [ ] **ADR-0002** — Reverb tail strategy (cabin IR characterization, AEC3 tail config)
2. [ ] **ADR-0003** — Canonical sample rate (16 kHz vs 32 kHz vs 48 kHz)  *(blocking Phase 0.5)*
3. [ ] **ADR-0004** — Mic geometry & beamforming  *(blocking Phase 0.5 if multi-mic confirmed)*
4. [ ] **ADR-0005** — Render-tap policy & DRC/EQ compensation  *(highest-impact unknown)*
5. [x] **ADR-0006** — AecChain interface alignment with WebRTC APM (created 2026-05-10; supersedes the earlier reservation of this number for Media-aware AEC)
6. [ ] **ADR-0007** — Neural runtime (TFLite vs ONNX vs raw weights; quantization plan)
7. [ ] **ADR-0008** — DSP offload decision criteria (when does Phase 6 trigger?)
8. [ ] **ADR-0009** — Media-aware AEC (music vs voice render) *(was previously slated as ADR-0006; renumbered when 0006 was used for the AecChain interface ADR)*

Process actions:

- [ ] Audit `webrtc::AudioProcessing` API vs current `AecChain` interface (30 min); update interface before Phase 0.5 lands.
- [ ] License audit for DTLN-AEC, NKF-AEC, athena-signal before any code is linked into shipped binary.
- [ ] HAL conversation with U300 platform: render-tap point, mic count + geometry, threading model, sample-rate fixed or negotiable.
- [ ] Define use-case explicitly: hands-free during media playback? Multi-zone? VoIP only? — drives A2, A3, A5.

## Status of dependent decisions

| Dependency | Affects | Locked? |
|---|---|---|
| Sample rate | All buffer sizes, all NN model rates, AEC3 config | **Not locked** (open A2) |
| Mic count / channel count | `Frame` shape, every API in `AecChain`, beamformer stage | **Not locked** (open A3) |
| Render tap point | AEC quality ceiling | **Not locked** (open A4) |
| Neural runtime | Build system, model conversion pipeline | **Not locked** (open ADR-0007) |
| DSP offload | Module split, threading | Locked deferred |

## References

- [PROJECT.md](../../PROJECT.md) — current authoritative description of the architecture
- [docs/deep-research-report.md](../deep-research-report.md) — Chinese-language research report, primary source
- [docs/Cellular Audio Processing Solutions Deep Dive.md](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — English counterpart, vendor IP focus
- [src/pipeline/aec_chain.h](../../src/pipeline/aec_chain.h) — interface as currently encoded
- [vendor/webrtc-audio-processing/webrtc/modules/audio_processing/include/audio_processing.h](../../vendor/webrtc-audio-processing/webrtc/modules/audio_processing/include/audio_processing.h) — WebRTC APM API (after `scripts/fetch-vendor.sh required`)

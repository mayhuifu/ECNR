# ADR-0006: AecChain interface alignment with WebRTC APM

**Status:** Accepted (Phase 0.5 — drives Task 2 interface expansion)
**Date:** 2026-05-10
**Deciders:** Project lead

## Context

ADR-0001 open assumption **A6** flagged that the current Phase-0 [`AecChain`](../../src/pipeline/aec_chain.h) interface — `Init / ProcessRender / ProcessCapture / Reset / Stats` — was sketched against a *stub* backend without an API audit of the real WebRTC `AudioProcessing` (APM) module that Phase 0.5 wires in. Before any wiring lands we need to know:

1. Which APM hooks our chain must mirror or wrap so they are reachable from callers.
2. Which APM hooks we deliberately drop, and why.
3. What the post-Phase-0.5 `AecChain` public surface should look like.

Source of truth read end-to-end:
- `vendor/webrtc-audio-processing/webrtc/api/audio/audio_processing.h` (956 lines — the `include/...` path is a forwarder to this file).
- `vendor/webrtc-audio-processing/webrtc/api/audio/audio_processing_statistics.h` (`AudioProcessingStats`).

Phase 0.5 ships a *baseline* AEC chain only: AEC3 enabled, NS off (RNNoise lives outside APM), AGC off, single-channel, 16 kHz, 10 ms frames. Decisions below are scoped to that baseline; entries marked *(deferred)* are recorded so Task 2 leaves room without implementing them.

## Decision

### 1. Methods APM exposes that `AecChain` should mirror or wrap

| APM surface (line) | Our wrapper | Why |
|---|---|---|
| `AudioProcessingBuilder::Create()` + `ApplyConfig(Config)` (l. 523, 829) | Already covered by `AecChain::Init(int sample_rate_hz)`. Internally Phase 0.5 sets `config.echo_canceller.enabled=true`, `mobile_mode=false`, `noise_suppression.enabled=false`, `gain_controller1.enabled=false`, `gain_controller2.enabled=false`, `high_pass_filter.enabled=true` (cheap and removes DC drift that hurts the adaptive filter). | The Phase-0 stub took only a sample rate; APM needs an explicit config struct. We hide the `webrtc::AudioProcessing::Config` type entirely — the only knobs Phase 0.5 callers can touch are sample rate and (next bullet) reset. |
| `Initialize()` (l. 508) | `AecChain::Reset()` re-uses it. | APM's contract: `Initialize()` resets internal state while keeping config. Maps 1:1 onto our `Reset()` semantic ("drop adapted state"). |
| `ProcessReverseStream(int16_t*, StreamConfig, ...)` (l. 571) | `AecChain::ProcessRender(const Frame&)`. | Same call ordering (render before matching capture). `Frame` is mono int16 today; we feed APM via the int16 interleaved overload because it matches our buffer format and avoids an int16->float conversion in the hot path. |
| `ProcessStream(int16_t*, ...)` (l. 551) | `AecChain::ProcessCapture(const Frame&, Frame& out)`. | Same as above for capture. APM permits in-place (`src == dest`); we keep the two-buffer signature so the `out` Frame can carry post-AEC stats and so callers can reason about lifetimes. |
| `set_stream_delay_ms(int)` (l. 620) | **NEW: `AecChain::SetStreamDelayMs(int)`.** | A6 flagged this as the most likely missing hook. AEC3 needs a coarse render→capture delay seed; if the HAL knows it (e.g. ALSA reports playback+capture latency) we propagate it. If we don't know it, we don't call it — APM's adaptive delay estimator handles the rest. Our wrapper accepts a value, clamps to `[0, 500]` ms, and forwards. Name chosen for parity with the underlying APM method `set_stream_delay_ms` (so a future reader searching the codebase for the WebRTC name finds our wrapper) and for consistency with ADR-0001 assumption A6 and the Phase 0.5 plan. |
| `GetStatistics()` → `AudioProcessingStats` (l. 672, statistics header l. 23) | **EXTEND `ChainStats`** with: `residual_echo_likelihood`, `residual_echo_likelihood_recent_max`, `delay_median_ms`, `delay_ms`, `echo_return_loss_db`, `divergent_filter_fraction`. Existing `erle_db` becomes the surfaced `echo_return_loss_enhancement` (already 10·log₁₀ as we defined it). | These are the proof points for the Phase-0.5 test thresholds (the plan calls out `residual_echo_likelihood` specifically). All exposed as `std::optional<...>` to mirror APM's contract that "not yet computed" is distinct from "zero". |
| `GetConfig()` (l. 681) | *(deferred — internal debug only)* — not on public surface for Phase 0.5. | We own the config; round-tripping it through the public API invites callers to fiddle with AEC3 internals. Reserved for an internal `AecChain::Impl` debug accessor. |

### 2. Methods APM exposes that we explicitly choose NOT to expose

| APM surface (line) | Decision | Rationale |
|---|---|---|
| `set_stream_analog_level(int)` / `recommended_stream_analog_level()` (l. 599, 605) | **Drop.** | We run AGC as a separate pipeline stage (PROJECT.md / ADR-0001 — "AGC after NS, after AEC"). Phase 0.5 disables APM's AGC entirely; analog-level coupling is meaningless when AGC is off, and propagating it would hard-couple the chain to the HAL's mic-volume control we explicitly don't own. If/when we move AGC into APM, this decision flips. |
| `Config::GainController1` / `GainController2` (l. 235, 338) | **Drop from public knobs.** | AGC is out of scope for Phase 0.5; AGC2 mid-pipeline conflicts with our post-chain AGC. Hard-coded `enabled=false`. |
| `Config::NoiseSuppression` (l. 216) | **Drop from public knobs.** | RNNoise runs as a downstream stage in our chain; APM's NS would be redundant and would mask the input RNNoise sees. Hard-coded `enabled=false`. |
| `Config::TransientSuppression` (l. 225) | **Drop.** | Marked deprecated in the header. Don't propagate. |
| `Config::PreAmplifier` / `CaptureLevelAdjustment` (l. 172, 179) | **Drop.** | Pre-gain is a HAL concern in our architecture. Adding it here makes input-level reasoning ambiguous. |
| `set_stream_key_pressed(bool)` (l. 625) | **Drop.** | Designed for VoIP UI ("user is typing"); irrelevant in-cabin. |
| `set_output_will_be_muted(bool)` (l. 539) | **Drop.** | Our chain output always feeds downstream stages; we never mute the chain output mid-stream. |
| `RuntimeSetting` / `SetRuntimeSetting` / `PostRuntimeSetting` (l. 389, 542, 546) | **Drop.** | Used for runtime gain tweaks under AGC; we don't expose AGC. The full `RuntimeSetting` surface is broad and pulls in `webrtc::` types we'd otherwise have to wrap one-for-one. |
| `CreateAndAttachAecDump` / `AttachAecDump` / `DetachAecDump` (l. 637, 664, 669) | **Drop from public surface.** | Useful for debugging but pulls in `TaskQueueBase` and `AecDump`. We add a project-internal `EnableAecDump(path)` later via an opt-in build flag, not as part of the stable interface. |
| `GetLinearAecOutput(...)` (l. 593) | **Drop.** | Phase 0.5 doesn't feed the linear-only AEC tap to a downstream consumer (RNNoise gets the full APM output, not the linear-only). Useful in Phase 3 if we run a neural RES that wants the linear residual; revisit then. |
| `AnalyzeReverseStream(...)` (l. 586) | **Drop.** | Analysis-only render path with no echo cancellation. We always cancel; we never just analyze. |
| `Float ProcessStream/ProcessReverseStream` overloads (l. 563, 578) | **Drop from our wrapper, but use internally if `Frame` becomes float.** | Today `Frame` is mono int16 — int16 APM overload matches. ADR-0003 (canonical sample rate) and a future float `Frame` could flip this; the wrapper hides the choice. |
| `CustomProcessing`, `CustomAudioAnalyzer`, `EchoDetector` injection (l. 749, 762, 930; builder l. 791, 798, 805, 812, 819) | **Drop.** | Custom-component injection at APM-build time is a power-user surface. Our chain has its own composition (AEC chain → RNNoise → AGC) outside APM; we don't need APM-internal custom stages. |
| `proc_sample_rate_hz()`, `num_*_channels()` accessors (l. 527–532) | **Drop.** | Internal to APM; we know what we configured because we configured it. Re-exposing invites drift. |

### 3. The expanded `AecChain` interface (header sketch for Task 2)

This is the **shape** Task 2 will implement against — not a working header. No `webrtc::` types appear in the public surface; all are wrapped or hidden behind `Impl`. `std::optional` is used where APM uses it, to preserve "not-yet-computed" vs "zero".

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/frame.h"

namespace ecnr {

// Aggregated runtime stats for the chain. Updated each ProcessCapture call.
// Fields named to mirror webrtc::AudioProcessingStats so the mapping is
// obvious to anyone holding the APM header open. Optional fields stay empty
// until APM has produced at least one valid measurement.
struct ChainStats {
  // ---- Cost / RTF (chain-owned, not from APM) ----
  double cpu_time_s   = 0.0;  // cumulative wallclock in ProcessRender + ProcessCapture
  double audio_time_s = 0.0;  // cumulative seconds of audio passed through

  double Rtf() const {
    return audio_time_s > 0.0 ? cpu_time_s / audio_time_s : 0.0;
  }

  // ---- Linear AEC quality (from APM GetStatistics) ----
  // ERLE = 10*log10(P_echo / P_out). Mirrors webrtc::AudioProcessingStats
  // ::echo_return_loss_enhancement (see audio_processing_statistics.h).
  // Renamed from Phase-0 'erle_db' for parity with the APM field name.
  std::optional<double> echo_return_loss_enhancement_db;
  // ERL = 10*log10(P_far / P_echo). Acoustic property of the room+speaker,
  // not the AEC. Mirrors webrtc::AudioProcessingStats::echo_return_loss.
  std::optional<double> echo_return_loss_db;
  // Fraction of the last 1 s window that AEC's adaptive filter was divergent.
  // Health signal; persistent > 0 means the AEC is unhappy (delay/clock drift).
  std::optional<double> divergent_filter_fraction;

  // ---- Residual-echo detector (Phase-0.5 test threshold lives here) ----
  std::optional<double> residual_echo_likelihood;
  std::optional<double> residual_echo_likelihood_recent_max;

  // ---- Delay estimator output ----
  std::optional<int32_t> delay_ms;          // instantaneous at GetStats time
  std::optional<int32_t> delay_median_ms;   // 1 s aggregation window
};

// AEC chain. Phase 0.5 backend: WebRTC AudioProcessing with AEC3 enabled,
// NS/AGC/transient-suppression disabled (those run as separate stages).
// 16 kHz / 10 ms / mono int16 frames in v1.
class AecChain {
 public:
  AecChain();
  ~AecChain();

  AecChain(const AecChain&) = delete;
  AecChain& operator=(const AecChain&) = delete;

  // Returns false if sample_rate_hz is unsupported (v1: 16 kHz only).
  // Builds the underlying webrtc::AudioProcessing with the Phase-0.5 baseline
  // config. Safe to call once; subsequent calls return false.
  bool Init(int sample_rate_hz);

  // Push a far-end (render / loudspeaker reference) frame. Must precede the
  // matching capture frame in time.
  void ProcessRender(const Frame& render);

  // Push a near-end (mic) frame; receive the cleaned frame in `out`.
  // `out` may alias `capture` — implementation honours APM's in-place contract.
  void ProcessCapture(const Frame& capture, Frame& out);

  // NEW (Task 2): hint the render->capture delay (ms) when the HAL knows it.
  // Forwards to webrtc::AudioProcessing::set_stream_delay_ms after clamping.
  // No-op if Init() has not been called. Safe to call between frames; do not
  // call concurrently with ProcessCapture.
  // Acceptable range: [0, 500] ms; values outside are clamped.
  void SetStreamDelayMs(int delay_ms);

  // Drop adapted state. Call on stream restart, sample-rate change, or
  // confirmed routing change at the HAL. Maps to APM Initialize().
  void Reset();

  const ChainStats& Stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr
```

Notes on the sketch:

1. The Phase-0 single `erle_db` field is **renamed** to `echo_return_loss_enhancement_db` and made `optional`. This is a breaking change to `ChainStats`. Phase 0.5 has one in-tree consumer (the harness/test that reports RTF and ERLE); Task 2 updates it in the same commit. No external API stability is owed at this stage of the project.
2. `Frame` stays mono int16 in v1. The ADR does not prejudge ADR-0003 (canonical sample rate) or ADR-0004 (multi-mic) — when those land, `Frame` widens and the wrapper switches to APM's float overload, but the *public* `AecChain` signatures above remain stable except for the channel count carried inside `Frame`.
3. `SetStreamDelayMs` is the only genuinely new public method. Everything else is rename / add-stat. This is the smallest expansion that covers A6 and unblocks the test threshold work in the rest of Phase 0.5. The name mirrors APM's `set_stream_delay_ms` verbatim.

## Consequences

**What becomes easier:**
- The Phase-0.5 test threshold ("residual echo likelihood < X after Y seconds") has a defined surface to read from — Task 5 of the plan can write its assertion against `Stats().residual_echo_likelihood_recent_max` without touching `webrtc::`.
- The render-tap delay story (ADR-0005) has an obvious propagation path: HAL exposes a delay → `SetStreamDelayMs(d)` → APM. No re-plumbing needed when Phase 1 wires the real HAL.
- New APM stats (e.g. `delay_median_ms`) added by future libwebrtc versions slot into `ChainStats` without touching the rest of the pipeline.

**What becomes harder:**
- Renaming `erle_db` is a one-time mechanical break for in-tree callers. Acceptable now; would be expensive if we delayed.
- `std::optional<double>` semantics in `ChainStats` mean every downstream consumer has to handle "not yet measured". This is correct (zero is not the same as silent), but it's more code than the Phase-0 plain-double struct.

**What we explicitly defer:**
- AGC integration (`set_stream_analog_level`, GainController config).
- AecDump debug recording — opt-in build flag later, not on the public surface.
- Multi-channel (`Frame` widening) — gated on ADR-0004.
- Float frame format — gated on ADR-0003.
- `GetLinearAecOutput` for neural RES — Phase 3, not now.

## Action items

- [ ] **Task 2 (Phase 0.5):** implement `aec_chain.h` to match the sketch above; update `ChainStats` and the single in-tree consumer; the implementation file (`aec_chain.cc`) holds the `webrtc::AudioProcessing` instance behind `Impl` and contains every `webrtc::` reference in the project.
- [ ] **Task 5 (Phase 0.5):** test threshold reads `Stats().residual_echo_likelihood_recent_max` and `Stats().echo_return_loss_enhancement_db`.
- [ ] When ADR-0005 (render-tap policy) lands, document where `SetStreamDelayMs` is called from (HAL adapter? a `DelayProvider` interface?).
- [ ] Close out ADR-0001 assumption A6 with a pointer to this ADR.

## References

- [src/pipeline/aec_chain.h](../../src/pipeline/aec_chain.h) — current Phase-0 interface this ADR expands.
- [vendor/webrtc-audio-processing/webrtc/api/audio/audio_processing.h](../../vendor/webrtc-audio-processing/webrtc/api/audio/audio_processing.h) — APM public API (the `modules/.../include/audio_processing.h` path is a forwarder to this file).
- [vendor/webrtc-audio-processing/webrtc/api/audio/audio_processing_statistics.h](../../vendor/webrtc-audio-processing/webrtc/api/audio/audio_processing_statistics.h) — `AudioProcessingStats` definition.
- [docs/adr/0001-hybrid-aec-architecture-review.md](0001-hybrid-aec-architecture-review.md) — open assumption A6 (audit the APM API).
- [docs/superpowers/plans/2026-05-10-phase-0.5-integration.md](../superpowers/plans/2026-05-10-phase-0.5-integration.md) — Phase 0.5 plan (this ADR is Task 1).

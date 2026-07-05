#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/frame.h"
#include "pipeline/aec_tuning.h"

namespace ecnr {

// Wraps webrtc::AudioProcessing configured for AEC3 only:
//   - echo cancellation enabled (mobile_mode = false; we're not running on
//     a phone with always-on noise floor; we want full AEC3)
//   - high-pass filter enabled (cheap; helps DC and low-frequency rumble)
//   - noise suppression DISABLED (RNNoise handles NS in Task 7)
//   - both gain controllers DISABLED (AGC is out of Phase 0.5 scope)
//
// Single-channel: the Beamformer upstream collapses N mics to mono before
// the chain reaches this adapter. Multi-rate: 16 kHz or 48 kHz at Init.
//
// All webrtc:: types stay in the .cc (hidden inside the Impl pimpl); callers
// see only ecnr:: types and the Stats struct below.
class Aec3Adapter {
 public:
  // Snapshot of AEC3-internal stats; copied into ChainStats by AecChain.
  struct Stats {
    std::optional<double> echo_return_loss_enhancement_db;
    std::optional<double> echo_return_loss_db;
    std::optional<double> residual_echo_likelihood;
    std::optional<double> residual_echo_likelihood_recent_max;
    std::optional<int32_t> delay_ms;
    std::optional<int32_t> delay_median_ms;
    std::optional<double> divergent_filter_fraction;
  };

  Aec3Adapter();
  ~Aec3Adapter();

  Aec3Adapter(const Aec3Adapter&) = delete;
  Aec3Adapter& operator=(const Aec3Adapter&) = delete;

  // sample_rate_hz must satisfy IsSupportedSampleRate (16000 or 48000).
  // Returns false on unsupported rate or if APM construction fails.
  bool Init(int sample_rate_hz);

  // Override AEC3's adaptive-filter length in 4 ms blocks for BOTH the
  // refined and coarse filters (WebRTC default: 13 blocks = 52 ms modelled
  // echo tail). Automotive cabins have short RT60 (~50 ms mid-band), so
  // shorter filters are a CPU lever in principle — filter apply + adapt
  // cost is linear in length. Measured 2026-07-04 on the host (NEON-off
  // APM build): cpu_aec is flat across 9-13 blocks, i.e. the host AEC3
  // budget is dominated by the matched-filter delay estimator + FFTs,
  // not the main filters. Retained as an ADR-0011 tuning knob for
  // Phase-2 cabin RT60 work (shorter tails also converge faster);
  // re-evaluate as a CPU lever on-target where NEON shifts the mix.
  // 0 = keep WebRTC defaults (production default).
  // **Must be called BEFORE Init()** — the config is baked into the
  // EchoControlFactory at APM construction. Values are clamped to
  // [9, 20] (below 9, APM's RenderDelayBuffer sizing aborts); the
  // initial-convergence filters are capped at the same length when it
  // drops below their 12-block default.
  void SetFilterLengthBlocks(int blocks);

  // Override AEC3 suppressor double-talk transparency (GB/T 45314 §5.7 —
  // see aec_tuning.h for field semantics). Sentinel-negative fields keep
  // WebRTC defaults. **Must be called BEFORE Init()** — baked into the
  // EchoControlFactory at APM construction, same as SetFilterLengthBlocks.
  void SetDtTuning(const AecDtTuning& tuning);

  // Drop AEC3 adapted state (re-runs APM Initialize). Not real-time safe —
  // call only between streams (session boundary), never on the audio thread
  // mid-frame.
  void Reset();

  // Forward to webrtc::AudioProcessing::set_stream_delay_ms (already
  // clamped to [0, kMaxStreamDelayMs] by the caller in AecChain).
  void SetStreamDelayMs(int ms);

  // Process a mono render frame (n_channels == 1, n_samples must match
  // the configured chain rate). Internal safety net only — the chain's
  // shape contract is enforced in AecChain. Misshape here is a no-op
  // (does NOT increment AecChain's frames_dropped counter).
  void ProcessRender(const Frame& render);

  // Process a mono capture frame (already post-beamformer). Writes the
  // AEC-cleaned mono signal into out (out.n_channels = 1, out.n_samples
  // = capture.n_samples). If APM construction failed or Init wasn't run,
  // out is set to a copy of capture (pass-through fallback).
  void ProcessCapture(const Frame& capture, Frame& out);

  // Snapshot of the latest APM stats. Cheap; returns by value. Optional
  // fields stay nullopt until APM has populated them (AEC3 needs ~1 s of
  // audio for ERLE / delay estimates).
  Stats GetStats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

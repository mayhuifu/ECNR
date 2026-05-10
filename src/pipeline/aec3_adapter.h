#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/frame.h"

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

  // Drop adapted state; call on stream restart, sample-rate change, or
  // confirmed routing change at the HAL.
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

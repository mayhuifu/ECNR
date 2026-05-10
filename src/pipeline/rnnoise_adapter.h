#pragma once

#include <memory>

#include "core/frame.h"

namespace ecnr {

// Wraps RNNoise (48 kHz / 480-sample frames, mono) for use inside AecChain.
// Bridges 16<->48 kHz via SpeexDSP resampler when the chain is at 16 kHz.
//
// Single-channel: input is post-AEC mono. Multi-rate: 16 kHz or 48 kHz at Init.
//
// Per ADR-0003 + ADR-0006, RNNoise replaces the Phase-0 StubNs as the noise
// suppressor in AecChain.
//
// All third-party types (RNNoise's DenoiseState, SpeexDSP's
// SpeexResamplerState) stay in the .cc inside the opaque Impl pimpl; callers
// see only ecnr:: types.
class RnNsAdapter {
 public:
  RnNsAdapter();
  ~RnNsAdapter();

  RnNsAdapter(const RnNsAdapter&) = delete;
  RnNsAdapter& operator=(const RnNsAdapter&) = delete;

  // sample_rate_hz must satisfy IsSupportedSampleRate (16000 or 48000).
  // Returns false on unsupported rate or if any sub-component fails.
  bool Init(int sample_rate_hz);

  // Drop adapted state and re-allocate the RNNoise context. Not real-time
  // safe — call only between streams (session boundary), never on the audio
  // thread mid-frame.
  void Reset();

  // Cap NS suppression by mixing α·input back into the (1−α)·rnnoise_output.
  // blend ∈ [0, 1]; 0 = current behavior (full RNNoise), 1 = NS bypass
  // (output equals input). With blend = 0.25 the effective suppression floor
  // is roughly -12 dB because the input always contributes ~25% of the
  // output, preventing RNNoise's mask from driving voice-bearing bands
  // arbitrarily close to silence (the "watery / chopped voice" artifact on
  // non-stationary noise scenes — babble, stadium, music — that motivates
  // PROJECT.md's "Known limitations" entry). Default 0.0 keeps the
  // pre-mitigation behavior so all existing test thresholds still hold.
  // Real-time safe; can be called between frames.
  void SetDryBlend(float blend);

  // Currently-set blend value. Useful for round-trip tests and bench logging.
  float DryBlend() const;

  // In-place processing of a mono frame. f.n_channels must be 1; f.n_samples
  // must match the configured rate. No-op (and stderr warning) on misshape.
  void Process(Frame& f);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

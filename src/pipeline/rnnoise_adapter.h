#pragma once

#include "core/frame.h"

// Forward-declare RNNoise + Speex types so callers don't pull in those headers.
struct DenoiseState;          // RNNoise's opaque state (typedef'd to DenoiseState in rnnoise.h)
struct SpeexResamplerState_;  // SpeexDSP forward decl (the leading underscore matches the typedef pattern)
typedef struct SpeexResamplerState_ SpeexResamplerState;

namespace ecnr {

// Wraps RNNoise (48 kHz / 480-sample frames, mono) for use inside AecChain.
// Bridges 16<->48 kHz via SpeexDSP resampler when the chain is at 16 kHz.
//
// Single-channel: input is post-AEC mono. Multi-rate: 16 kHz or 48 kHz at Init.
//
// Per ADR-0003 + ADR-0006, RNNoise replaces the Phase-0 StubNs as the noise
// suppressor in AecChain.
class RnNsAdapter {
 public:
  RnNsAdapter();
  ~RnNsAdapter();

  RnNsAdapter(const RnNsAdapter&) = delete;
  RnNsAdapter& operator=(const RnNsAdapter&) = delete;

  // sample_rate_hz must satisfy IsSupportedSampleRate (16000 or 48000).
  // Returns false on unsupported rate or if any sub-component fails.
  bool Init(int sample_rate_hz);

  // Drop adapted state (RNNoise internal model context + resampler memory).
  void Reset();

  // In-place processing of a mono frame. f.n_channels must be 1; f.n_samples
  // must match the configured rate. No-op (and stderr warning) on misshape.
  void Process(Frame& f);

 private:
  DenoiseState* st_ = nullptr;          // RNNoise state
  SpeexResamplerState* up_ = nullptr;   // 16k -> 48k (only at 16k tier)
  SpeexResamplerState* down_ = nullptr; // 48k -> 16k (only at 16k tier)
  int sample_rate_hz_ = 0;
};

}  // namespace ecnr

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
  // Uniform blend (same α for every frame). blend ∈ [0, 1]; 0 = current
  // behavior (full RNNoise), 1 = NS bypass (output equals input). With
  // blend = 0.25 the effective suppression floor is roughly -12 dB because
  // the input always contributes ~25% of the output. Internally this calls
  // SetVadBlendRange(blend, blend) — uniform blend is a special case of
  // VAD-gated blend with equal endpoints.
  void SetDryBlend(float blend);

  // VAD-gated blend (Step B mitigation). Selects per-frame α between
  // `low` (noise-dominant frames) and `high` (voice-dominant frames),
  // interpolated by RNNoise's own voice-activity probability output (the
  // return value of rnnoise_process_frame). Asymmetric smoothing: fast
  // attack to voice mode (protects speech onsets from missed VAD),
  // slow decay back to noise mode (~140 ms half-life — keeps voice mode
  // through brief inter-word silences). low = high reduces to a uniform
  // blend; low = 0.0, high = 0.30 is the suggested production default:
  // noise-only frames get full RNNoise, voice-dominant frames get ~-10 dB
  // suppression cap so voice content isn't chopped. Real-time safe; can
  // be called between frames.
  void SetVadBlendRange(float low, float high);

  // Currently-active blend for the most recent processed frame (after
  // VAD smoothing and interpolation). Useful for bench-line logging and
  // for AecChain to surface into ChainStats. Returns the configured
  // low value before the first Process() call.
  float CurrentBlend() const;

  // Most recent RNNoise-reported voice activity probability (smoothed by
  // the asymmetric hangover). [0, 1]. Useful diagnostic for the bench
  // and live binaries.
  float LastVadProb() const;

  // In-place processing of a mono frame. f.n_channels must be 1; f.n_samples
  // must match the configured rate. No-op (and stderr warning) on misshape.
  void Process(Frame& f);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

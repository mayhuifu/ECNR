#pragma once

#include <memory>
#include <optional>

#include "core/frame.h"

namespace ecnr {

// Wraps webrtc::AudioProcessing configured for AGC2 (adaptive digital gain)
// only — all other APM stages (AEC, NS, HPF) are off.
//
// Why a separate APM instance from Aec3Adapter: the chain's architecture
// (ADR-0001) places AGC AFTER NS, so AGC has to run on the post-NS output,
// downstream of RnNsAdapter. Stuffing AGC2 into the existing AEC3 APM would
// run it pre-NS, which is the wrong placement for VoLTE/VoNR uplink-level
// compliance (3GPP TS 26.131 sends-loudness rating is measured on the final
// transmitted signal).
//
// Single-channel: post-NS mono. Multi-rate: 16 kHz or 48 kHz at Init.
//
// The APM type stays inside the Impl pimpl; callers see only ecnr:: types.
class Agc2Adapter {
 public:
  // Snapshot of AGC2 stats; copied into ChainStats by AecChain.
  struct Stats {
    // Most recent applied digital gain in dB. Useful for confirming AGC2
    // is actually doing work (i.e., changing the signal level).
    std::optional<float> applied_gain_db;
  };

  Agc2Adapter();
  ~Agc2Adapter();

  Agc2Adapter(const Agc2Adapter&) = delete;
  Agc2Adapter& operator=(const Agc2Adapter&) = delete;

  // sample_rate_hz must satisfy IsSupportedSampleRate (16000 or 48000).
  // max_gain_db caps the adaptive-digital gain — defaults to 50 dB
  // (WebRTC's default). Lower values cap noise-floor amplification
  // between speech bursts; useful when default AGC over-brightens
  // background noise on already-quiet output. Range: 0 to 100 dB.
  // Returns false on unsupported rate or if APM construction fails.
  bool Init(int sample_rate_hz, float max_gain_db = 50.0f);

  // Drop adapted state (re-runs APM Initialize). Not real-time safe — call
  // only between streams (session boundary), never on the audio thread
  // mid-frame.
  void Reset();

  // In-place processing of a mono frame. f.n_channels must be 1; f.n_samples
  // must match the configured rate. No-op on misshape (does NOT bump
  // AecChain's frames_dropped counter — internal safety net only).
  void Process(Frame& f);

  // Snapshot of the latest AGC2 stats. Cheap; returns by value.
  Stats GetStats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

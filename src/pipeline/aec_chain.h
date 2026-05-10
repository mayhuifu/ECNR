#pragma once

#include <cstdint>
#include <memory>

#include "core/frame.h"

namespace ecnr {

// Aggregated runtime stats for the chain. Updated each ProcessCapture call.
struct ChainStats {
  // Echo Return Loss Enhancement (dB), instantaneous over the last frame.
  // Defined as 10 * log10(E_capture / E_residual). Higher = more echo removed.
  // Set to 0.0 if either signal is silent.
  double erle_db = 0.0;
  // Cumulative wallclock seconds spent in ProcessRender + ProcessCapture.
  double cpu_time_s = 0.0;
  // Cumulative seconds of audio passed through the chain.
  double audio_time_s = 0.0;

  // Real-Time Factor: cpu_time / audio_time. < 1.0 = faster than realtime.
  double Rtf() const {
    return audio_time_s > 0.0 ? cpu_time_s / audio_time_s : 0.0;
  }
};

// AEC + NS chain. Phase 0 ships stub backends behind this interface; Phase 0.5
// swaps in WebRTC AEC3 + RNNoise without changing callers.
class AecChain {
 public:
  AecChain();
  ~AecChain();

  AecChain(const AecChain&) = delete;
  AecChain& operator=(const AecChain&) = delete;

  // Sample rate must equal kSampleRateHz for v1 (16 kHz, 10 ms frames).
  // Returns false if sample_rate_hz is unsupported.
  bool Init(int sample_rate_hz);

  // Push a far-end (render / loudspeaker reference) frame. Must precede the
  // matching capture frame in time. The chain's delay estimator handles
  // arbitrary mic-vs-render latency at the system boundary.
  void ProcessRender(const Frame& render);

  // Push a near-end (mic) frame; receive the cleaned frame in `out`.
  void ProcessCapture(const Frame& capture, Frame& out);

  // Drop adapted state — call on stream restart, sample-rate change, or
  // confirmed routing change at the HAL.
  void Reset();

  const ChainStats& Stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

#pragma once

#include <array>
#include <cstdint>

namespace ecnr {

// 10 ms mono frame at 16 kHz.
//
// Why this fixed shape: WebRTC AEC3 processes 10 ms blocks natively; the rest
// of the chain (delay estimator, mode controller) assumes the same cadence.
// Other sample rates flow through the resampler at the chain edges.
inline constexpr int kSampleRateHz = 16000;
inline constexpr int kFrameDurationMs = 10;
inline constexpr int kFrameSamples = kSampleRateHz * kFrameDurationMs / 1000;  // 160

struct Frame {
  std::array<int16_t, kFrameSamples> samples{};
};

}  // namespace ecnr

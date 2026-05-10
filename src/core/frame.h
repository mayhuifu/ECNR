#pragma once

#include <array>
#include <cstdint>

namespace ecnr {

// 10 ms multi-channel frame. Sample rate is runtime (set at AecChain::Init);
// channel count is runtime in [1, kMaxMics].
//
// Per ADR-0003 (canonical sample rate, two-tier 16/48 kHz) and ADR-0004
// (mic array 2-8). Both tiers use 10 ms frames; AEC3 processes 10 ms blocks
// natively, the rest of the chain (delay estimator, mode controller) assumes
// the same cadence.
//
// Storage is sized for the maximum (kMaxMics × kFrameSamples48k) so a Frame is
// stack-allocatable and cache-friendly with no audio-thread allocations. Only
// the active subrange `[0, n_channels) × [0, n_samples)` is meaningful — the
// rest is uninitialized scratch and must not be read.
inline constexpr int kMaxMics = 8;
inline constexpr int kFrameDurationMs = 10;
inline constexpr int kFrameSamples16k = 160;
inline constexpr int kFrameSamples48k = 480;

struct Frame {
  int n_samples = kFrameSamples16k;            // 160 (16k) or 480 (48k)
  int n_channels = 1;                          // 1..kMaxMics
  std::array<std::array<int16_t, kFrameSamples48k>, kMaxMics> ch{};  // ch[c][s]
};

// Pure helpers used by AecChain::Init and Beamformer::Init to validate args.
inline constexpr bool IsSupportedSampleRate(int sample_rate_hz) {
  return sample_rate_hz == 16000 || sample_rate_hz == 48000;
}

inline constexpr int FrameSamplesFor(int sample_rate_hz) {
  return sample_rate_hz == 16000 ? kFrameSamples16k
       : sample_rate_hz == 48000 ? kFrameSamples48k
       : 0;  // unsupported; caller checks with IsSupportedSampleRate first
}

// ADR-0004: production mic count is [2, 8]. 1 is rejected (degenerate
// beamformer; out of scope) and >8 is out of contract.
inline constexpr bool IsSupportedMicCount(int num_mics) {
  return num_mics >= 2 && num_mics <= kMaxMics;
}

}  // namespace ecnr

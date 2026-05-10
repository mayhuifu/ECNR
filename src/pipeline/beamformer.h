#pragma once

#include <array>
#include <cstdint>

#include "core/frame.h"
#include "pipeline/mic_geometry.h"

namespace ecnr {

// Per ADR-0010: Phase 1 ships a fixed delay-and-sum (DSB) beamformer.
// Per-mic delays are computed once at Init from MicGeometry + speed of
// sound; the runtime path is N fractional-delay-aligned channels summed
// with 1/N weighting. No adaptation; deterministic frame-to-frame.
//
// The two-arg Init overload (preserved for ADR-0004 callers) forwards to
// the three-arg form with kPassthroughGeometry; passthrough geometry takes
// the ch[0]-verbatim fast path, identical to the Phase 0.5 stub.
class Beamformer {
 public:
  Beamformer();
  ~Beamformer();

  Beamformer(const Beamformer&) = delete;
  Beamformer& operator=(const Beamformer&) = delete;

  // Existing (ADR-0004): geometry-less init. Behaves identically to
  // Init(rate, num_mics, kPassthroughGeometry) — selects ch[0] verbatim.
  bool Init(int sample_rate_hz, int num_mics);

  // New (ADR-0010): geometry-aware init. Required for DSB / MVDR / GSC.
  // Returns false if sample_rate or num_mics are out of contract (see
  // IsSupportedSampleRate / IsSupportedMicCount), if the geometry has
  // degenerate positions (two mics at the same coordinate AND a
  // non-passthrough target direction), or if target_direction is the
  // zero vector. When geometry == kPassthroughGeometry, takes the
  // passthrough fast path with no per-mic delay computation.
  bool Init(int sample_rate_hz, int num_mics, const MicGeometry& geometry);

  // multi -> mono. Asserts mic_in.n_channels == configured num_mics and
  // mic_in.n_samples == configured frame samples.
  //
  // In passthrough mode: mono_out is set to n_channels=1 with samples
  // drawn from ch[0] verbatim.
  //
  // In DSB mode: per-mic samples are advanced/delayed (linear-interpolated
  // for the fractional component) to align in the target_direction, then
  // averaged. The first call(s) after Init/Reset draw "history" samples
  // from a zero-initialized per-mic buffer, so the first few output
  // samples have transient attenuation; this is bounded by max-delay
  // samples and is sub-millisecond at typical cabin spacings.
  //
  // Bit-identical-channel detection: when DSB is active and num_mics >= 2
  // AND ch[0] and ch[1] are bit-identical for the first frame, a one-shot
  // warning is logged to stderr. This catches the Phase-0.5 mono-
  // duplication harness pattern continuing past its intended lifespan
  // (see ADR-0010 §5).
  void Process(const Frame& mic_in, Frame& mono_out);

  void Reset();

 private:
  // Maximum delay-buffer depth per channel. Sized for the worst case at
  // 48 kHz with a generous cabin array (8 mics in ~30 cm aperture:
  // 30cm / 343 m/s ≈ 0.87 ms ≈ 42 samples at 48 kHz). 64 leaves headroom.
  static constexpr int kMaxDelaySamples = 64;

  int sample_rate_hz_ = 0;
  int num_mics_ = 0;
  bool is_passthrough_ = true;

  // Per-mic alignment delay, in fractional samples. delay_samples_[c] =
  // integer_delay_[c] + fractional_delay_[c] (fractional in [0, 1)).
  // Always >= 0; min delay across mics is 0 (the one closest to source).
  std::array<int, kMaxMics> integer_delay_{};
  std::array<float, kMaxMics> fractional_delay_{};

  // Per-mic history of the last kMaxDelaySamples input samples. Zero-
  // initialized at Reset; refilled with the tail of each frame after
  // processing. Indexed [0..kMaxDelaySamples-1] where [kMaxDelaySamples-1]
  // is the most recent past sample (sample at relative index -1 from the
  // current frame).
  std::array<std::array<int16_t, kMaxDelaySamples>, kMaxMics> history_{};

  // One-shot guard for the bit-identical-channel warning.
  bool warned_bit_identical_ = false;
  bool checked_bit_identical_ = false;
};

}  // namespace ecnr

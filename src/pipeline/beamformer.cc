#include "pipeline/beamformer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "core/frame.h"
#include "pipeline/mic_geometry.h"

namespace ecnr {
namespace {

// Treat a position vector as "at origin" if all components are exactly zero.
// kPassthroughGeometry zero-initializes positions_m, so the default-
// constructed sentinel hits this. Real geometries supplied by the HAL
// will always have at least one nonzero coordinate per mic.
bool AllAtOrigin(const MicGeometry& g, int num_mics) {
  for (int c = 0; c < num_mics; ++c) {
    if (g.positions_m[c][0] != 0.0f ||
        g.positions_m[c][1] != 0.0f ||
        g.positions_m[c][2] != 0.0f) {
      return false;
    }
  }
  return true;
}

float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool HasDuplicateMic(const MicGeometry& g, int num_mics) {
  for (int i = 0; i < num_mics; ++i) {
    for (int j = i + 1; j < num_mics; ++j) {
      if (g.positions_m[i][0] == g.positions_m[j][0] &&
          g.positions_m[i][1] == g.positions_m[j][1] &&
          g.positions_m[i][2] == g.positions_m[j][2]) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

Beamformer::Beamformer() = default;
Beamformer::~Beamformer() = default;

bool Beamformer::Init(int sample_rate_hz, int num_mics) {
  return Init(sample_rate_hz, num_mics, kPassthroughGeometry);
}

bool Beamformer::Init(int sample_rate_hz, int num_mics,
                      const MicGeometry& geometry) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;
  if (!IsSupportedMicCount(num_mics)) return false;
  if (geometry.speed_of_sound_mps <= 0.0f) return false;

  const float dir_norm_sq =
      Dot3(geometry.target_direction, geometry.target_direction);
  if (dir_norm_sq <= 0.0f) return false;

  sample_rate_hz_ = sample_rate_hz;
  num_mics_ = num_mics;
  Reset();

  if (AllAtOrigin(geometry, num_mics)) {
    // Passthrough fast path: ch[0] verbatim. Delays are all zero; we never
    // enter the DSB hot loop.
    is_passthrough_ = true;
    return true;
  }

  if (HasDuplicateMic(geometry, num_mics)) return false;

  // Compute per-mic alignment delays. For a plane wave arriving from
  // target_direction (source at +d̂ from array origin), mic c at p_c
  // receives the wavefront with a relative lead of τ_c = (p_c · d̂)/c_s
  // seconds — i.e., x_c(t) = s(t + τ_c) where s(t) is the signal at the
  // origin. To time-align all mics to a common reference (the rearmost
  // mic, the one with the smallest τ_c), each mic must be delayed by
  // Δ_c = τ_c - τ_min. The mic with the most-negative projection (the
  // rearmost in d̂ direction) gets 0 delay; the mic closest to the source
  // gets the largest positive delay. By convention ch[0] should be the
  // rearmost mic so it remains the zero-delay channel — but the
  // implementation handles any ordering.
  const float dir_norm = std::sqrt(dir_norm_sq);
  const std::array<float, 3> dhat = {
      geometry.target_direction[0] / dir_norm,
      geometry.target_direction[1] / dir_norm,
      geometry.target_direction[2] / dir_norm,
  };

  std::array<float, kMaxMics> tau{};
  float tau_min = std::numeric_limits<float>::infinity();
  for (int c = 0; c < num_mics; ++c) {
    tau[c] = Dot3(geometry.positions_m[c], dhat) / geometry.speed_of_sound_mps;
    if (tau[c] < tau_min) tau_min = tau[c];
  }

  for (int c = 0; c < num_mics; ++c) {
    const float delay_seconds = tau[c] - tau_min;                       // >= 0
    const float delay_samples = delay_seconds * sample_rate_hz_;        // >= 0
    int int_part = static_cast<int>(std::floor(delay_samples));
    float frac_part = delay_samples - static_cast<float>(int_part);
    // Numerical safety: clamp to [0, kMaxDelaySamples - 1]. Geometries
    // bigger than the cabin (e.g., 30+ cm at 48 kHz) would saturate; we
    // already reject them implicitly because tau_max - tau[c] grows with
    // aperture, and the saturating clamp is preferable to a buffer
    // overrun on a misconfigured HAL.
    if (int_part < 0) { int_part = 0; frac_part = 0.0f; }
    if (int_part >= kMaxDelaySamples) {
      int_part = kMaxDelaySamples - 1;
      frac_part = 0.0f;
    }
    integer_delay_[c] = int_part;
    fractional_delay_[c] = frac_part;
  }

  is_passthrough_ = false;
  return true;
}

void Beamformer::Process(const Frame& mic_in, Frame& mono_out) {
  assert(mic_in.n_channels == num_mics_);
  assert(mic_in.n_samples == FrameSamplesFor(sample_rate_hz_));
  if (mic_in.n_channels != num_mics_ ||
      mic_in.n_samples != FrameSamplesFor(sample_rate_hz_)) {
    std::fprintf(stderr,
        "Beamformer::Process: dropping frame (n_channels=%d expected=%d, n_samples=%d expected=%d)\n",
        mic_in.n_channels, num_mics_, mic_in.n_samples,
        FrameSamplesFor(sample_rate_hz_));
    mono_out.n_channels = 1;
    mono_out.n_samples = 0;
    return;
  }

  const int n = mic_in.n_samples;

  // Passthrough fast path — preserves bit-equality with ch[0] (test #4 of
  // ADR-0010 verifies this). No history update needed; passthrough has no
  // delay state.
  if (is_passthrough_) {
    mono_out.n_channels = 1;
    mono_out.n_samples = n;
    std::memcpy(mono_out.ch[0].data(), mic_in.ch[0].data(),
                static_cast<size_t>(n) * sizeof(int16_t));
    return;
  }

  // Bit-identical-channel check (one-shot, first non-passthrough Process
  // call only). Catches the Phase-0.5 harness pattern of duplicating mono
  // into ch[0] and ch[1] — degenerate for any real beamformer.
  if (!checked_bit_identical_ && num_mics_ >= 2) {
    checked_bit_identical_ = true;
    if (std::memcmp(mic_in.ch[0].data(), mic_in.ch[1].data(),
                    static_cast<size_t>(n) * sizeof(int16_t)) == 0) {
      warned_bit_identical_ = true;
      std::fprintf(stderr,
          "Beamformer: WARNING — ch[0] and ch[1] are bit-identical on the "
          "first non-passthrough frame. This is degenerate for DSB (zero "
          "inter-mic delay; perfectly correlated channels). If you are "
          "feeding mono input duplicated into multiple channels, pass "
          "--bypass-beamformer or use real multi-channel capture. "
          "(One-shot warning per Beamformer instance.)\n");
    }
  }

  // DSB: per-output-sample, accumulate the linearly-interpolated, delay-
  // aligned sample from each mic, then divide by N.
  const float inv_n = 1.0f / static_cast<float>(num_mics_);
  for (int s = 0; s < n; ++s) {
    float acc = 0.0f;
    for (int c = 0; c < num_mics_; ++c) {
      const int idx = s - integer_delay_[c];
      // Fetch samples at idx and idx-1 (the linear-interp neighbours).
      // Negative idx reads from history; non-negative from the current frame.
      auto fetch = [&](int i) -> int16_t {
        if (i >= 0) return mic_in.ch[c][i];
        const int h = kMaxDelaySamples + i;  // i in [-kMaxDelaySamples, -1]
        if (h < 0) return 0;                 // beyond history (shouldn't happen)
        return history_[c][h];
      };
      const int16_t a = fetch(idx);
      const int16_t b = fetch(idx - 1);
      const float frac = fractional_delay_[c];
      acc += (1.0f - frac) * static_cast<float>(a) + frac * static_cast<float>(b);
    }
    acc *= inv_n;
    // int16 saturation. Round-to-nearest via lrintf is over-engineered
    // here; truncation toward zero (cast) is fine for an average and
    // matches the WebRTC AEC3 input handling.
    if (acc > 32767.0f) acc = 32767.0f;
    if (acc < -32768.0f) acc = -32768.0f;
    mono_out.ch[0][s] = static_cast<int16_t>(acc);
  }
  mono_out.n_channels = 1;
  mono_out.n_samples = n;

  // Refresh history with the last kMaxDelaySamples samples of this frame.
  // n is always >> kMaxDelaySamples (160 or 480 vs 64), so the simple tail
  // copy is always sufficient.
  for (int c = 0; c < num_mics_; ++c) {
    std::memcpy(history_[c].data(),
                mic_in.ch[c].data() + (n - kMaxDelaySamples),
                static_cast<size_t>(kMaxDelaySamples) * sizeof(int16_t));
  }
}

void Beamformer::Reset() {
  for (auto& h : history_) h.fill(0);
  checked_bit_identical_ = false;
  warned_bit_identical_ = false;
}

}  // namespace ecnr

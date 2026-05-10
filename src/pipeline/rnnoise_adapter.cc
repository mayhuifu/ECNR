#include "pipeline/rnnoise_adapter.h"

#include <rnnoise.h>
#include <speex/speex_resampler.h>

#include <array>
#include <cstdint>
#include <cstdio>

namespace ecnr {

namespace {
constexpr int kRnnoiseFrameSamples = 480;  // 48 kHz / 100 Hz
constexpr int kSpeexQuality = 5;
}  // namespace

// Opaque Impl: holds all third-party state. Custom destructor frees the
// RNNoise context and the two SpeexDSP resamplers; std::unique_ptr<Impl> in
// RnNsAdapter then auto-cleans on outer destruction.
struct RnNsAdapter::Impl {
  DenoiseState* st = nullptr;          // RNNoise state
  SpeexResamplerState* up = nullptr;   // 16k -> 48k (only at 16k tier)
  SpeexResamplerState* down = nullptr; // 48k -> 16k (only at 16k tier)
  int sample_rate_hz = 0;
  // VAD-gated blend (Step B). vad_blend_low <= α(p) <= vad_blend_high where
  // p is RNNoise's smoothed voice-activity probability. Uniform blend
  // (Step A) is the special case low == high. Both default 0 = pre-
  // mitigation behaviour (full RNNoise on every frame).
  float vad_blend_low = 0.0f;
  float vad_blend_high = 0.0f;
  float vad_smoothed = 0.0f;           // smoothed voice prob [0, 1]
  float current_blend = 0.0f;          // last applied α (for diagnostics)
  // Asymmetric one-pole filter on RNNoise's voice probability. Attack pulls
  // hard toward p when p > smoothed (fast onset detection); decay pulls
  // gently back when p < smoothed (slow drift back through pauses, keeping
  // voice mode active through inter-word silences). Coefficients at 16 k
  // (10 ms frames): attack ≈ 50 % per frame → 10 ms half-life; decay
  // ≈ 5 % per frame → ~140 ms half-life. Reasonable for speech rhythms.
  float vad_attack = 0.5f;
  float vad_decay = 0.05f;

  ~Impl() {
    if (st) rnnoise_destroy(st);
    if (up) speex_resampler_destroy(up);
    if (down) speex_resampler_destroy(down);
  }
};

RnNsAdapter::RnNsAdapter() : impl_(std::make_unique<Impl>()) {}
RnNsAdapter::~RnNsAdapter() = default;  // unique_ptr<Impl> handles cleanup

bool RnNsAdapter::Init(int sample_rate_hz) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;
  // Tear down any previous state (Reset path or repeated Init).
  if (impl_->st) { rnnoise_destroy(impl_->st); impl_->st = nullptr; }
  if (impl_->up) { speex_resampler_destroy(impl_->up); impl_->up = nullptr; }
  if (impl_->down) { speex_resampler_destroy(impl_->down); impl_->down = nullptr; }

  impl_->st = rnnoise_create(/*model=*/nullptr);
  if (!impl_->st) return false;

  if (sample_rate_hz == 16000) {
    int err = 0;
    impl_->up = speex_resampler_init(/*channels=*/1, /*in=*/16000, /*out=*/48000,
                                     kSpeexQuality, &err);
    if (!impl_->up || err != 0) return false;
    impl_->down = speex_resampler_init(1, 48000, 16000, kSpeexQuality, &err);
    if (!impl_->down || err != 0) return false;
  }
  // 48 kHz tier: no resampling; up/down stay null.

  impl_->sample_rate_hz = sample_rate_hz;
  return true;
}

void RnNsAdapter::Reset() {
  if (impl_->st) {
    rnnoise_destroy(impl_->st);
    impl_->st = rnnoise_create(/*model=*/nullptr);
  }
  if (impl_->up) speex_resampler_reset_mem(impl_->up);
  if (impl_->down) speex_resampler_reset_mem(impl_->down);
  // Blend endpoints + smoothing constants are configuration and survive
  // Reset; the smoothed VAD probability is adaptive state and drops to 0
  // so a new session starts in noise-mode (consistent with first-frame
  // behaviour at construction time).
  impl_->vad_smoothed = 0.0f;
  impl_->current_blend = impl_->vad_blend_low;
}

void RnNsAdapter::SetDryBlend(float blend) {
  // Uniform blend is just VAD-gated blend with both endpoints equal.
  SetVadBlendRange(blend, blend);
}

void RnNsAdapter::SetVadBlendRange(float low, float high) {
  auto clamp01 = [](float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
  };
  impl_->vad_blend_low = clamp01(low);
  impl_->vad_blend_high = clamp01(high);
  // Keep current_blend in sync for diagnostic getters called before any
  // Process(); uses the low value as the "no-voice" baseline.
  impl_->current_blend = impl_->vad_blend_low;
}

float RnNsAdapter::CurrentBlend() const { return impl_->current_blend; }
float RnNsAdapter::LastVadProb() const { return impl_->vad_smoothed; }

void RnNsAdapter::Process(Frame& f) {
  if (!impl_->st) return;
  if (f.n_channels != 1 || f.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    std::fprintf(stderr,
        "RnNsAdapter::Process: dropping frame (n_channels=%d expected=1, n_samples=%d expected=%d)\n",
        f.n_channels, f.n_samples, FrameSamplesFor(impl_->sample_rate_hz));
    return;
  }

  // Save input for the optional wet/dry blend. Always done so the same code
  // path runs regardless of blend value (deterministic perf); the blend
  // arithmetic itself is skipped when dry_blend == 0 (most common config).
  // Sized for the max frame (48 kHz tier); only the first n_samples are read.
  std::array<int16_t, kFrameSamples48k> input_saved{};
  const int n = f.n_samples;
  for (int i = 0; i < n; ++i) input_saved[i] = f.ch[0][i];

  // Working buffer at 48 kHz.
  std::array<float, kRnnoiseFrameSamples> rnnoise_buf{};
  // RNNoise's per-frame voice-activity probability (function return value).
  // Used to modulate the wet/dry blend between vad_blend_low and
  // vad_blend_high.
  float vad_prob = 0.0f;

  if (impl_->sample_rate_hz == 48000) {
    // Direct: int16 -> float (int16-range, NOT normalized).
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      rnnoise_buf[i] = static_cast<float>(f.ch[0][i]);
    }
    vad_prob = rnnoise_process_frame(impl_->st, rnnoise_buf.data(),
                                     rnnoise_buf.data());
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      const float v = rnnoise_buf[i];
      f.ch[0][i] = static_cast<int16_t>(
          v < -32768.0f ? -32768 : v > 32767.0f ? 32767 : v);
    }
  } else {
    // 16 kHz tier: upsample to 48 kHz, process, downsample.
    // Step 1: int16 16k -> int16 48k.
    std::array<int16_t, kRnnoiseFrameSamples> up_buf{};
    spx_uint32_t in_len = static_cast<spx_uint32_t>(f.n_samples);          // 160
    spx_uint32_t out_len = kRnnoiseFrameSamples;                            // 480
    speex_resampler_process_int(impl_->up, /*channel=*/0,
                                f.ch[0].data(), &in_len,
                                up_buf.data(), &out_len);
    // Note: SpeexDSP may produce slightly fewer than 480 samples in a single
    // call due to internal latency; for the first ~few frames the resampler
    // is in warmup. Pad with zeros if short.
    for (spx_uint32_t i = out_len; i < kRnnoiseFrameSamples; ++i) {
      up_buf[i] = 0;
    }

    // Step 2: int16 48k -> float (int16-range), process, float -> int16.
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      rnnoise_buf[i] = static_cast<float>(up_buf[i]);
    }
    vad_prob = rnnoise_process_frame(impl_->st, rnnoise_buf.data(),
                                     rnnoise_buf.data());
    std::array<int16_t, kRnnoiseFrameSamples> processed_48k{};
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      const float v = rnnoise_buf[i];
      processed_48k[i] = static_cast<int16_t>(
          v < -32768.0f ? -32768 : v > 32767.0f ? 32767 : v);
    }

    // Step 3: int16 48k -> int16 16k (back to f.ch[0]).
    in_len = kRnnoiseFrameSamples;
    out_len = static_cast<spx_uint32_t>(f.n_samples);
    speex_resampler_process_int(impl_->down, 0,
                                processed_48k.data(), &in_len,
                                f.ch[0].data(), &out_len);
    // Same warmup caveat for downsample. Zero-pad short output.
    for (spx_uint32_t i = out_len; i < static_cast<spx_uint32_t>(f.n_samples); ++i) {
      f.ch[0][i] = 0;
    }
  }

  // Smooth VAD probability with asymmetric attack/decay so the blend
  // modulation doesn't pop at every short noisy frame.
  if (vad_prob > impl_->vad_smoothed) {
    impl_->vad_smoothed =
        (1.0f - impl_->vad_attack) * impl_->vad_smoothed +
        impl_->vad_attack * vad_prob;
  } else {
    impl_->vad_smoothed =
        (1.0f - impl_->vad_decay) * impl_->vad_smoothed +
        impl_->vad_decay * vad_prob;
  }
  // Compute the per-frame blend α by interpolating between the two
  // configured endpoints. When low == high this collapses to a uniform
  // (Step-A) blend and the VAD has no effect — bit-exact backward
  // compatibility for callers that only set SetDryBlend.
  const float alpha = impl_->vad_blend_low +
                      (impl_->vad_blend_high - impl_->vad_blend_low) *
                          impl_->vad_smoothed;
  impl_->current_blend = alpha;

  // Wet/dry blend: out = α·input + (1−α)·rnnoise_output. Skipped at α=0 for
  // bit-exact backward compatibility with pre-mitigation behavior.
  if (alpha > 0.0f) {
    const float wet = 1.0f - alpha;
    for (int i = 0; i < n; ++i) {
      const float v = alpha * static_cast<float>(input_saved[i]) +
                      wet   * static_cast<float>(f.ch[0][i]);
      f.ch[0][i] = static_cast<int16_t>(
          v < -32768.0f ? -32768 : v > 32767.0f ? 32767 : v);
    }
  }
}

}  // namespace ecnr

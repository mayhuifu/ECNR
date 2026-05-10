#include "pipeline/rnnoise_adapter.h"

#include <rnnoise.h>
#include <speex/speex_resampler.h>

#include <algorithm>
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
}

void RnNsAdapter::Process(Frame& f) {
  if (!impl_->st) return;
  if (f.n_channels != 1 || f.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    std::fprintf(stderr,
        "RnNsAdapter::Process: dropping frame (n_channels=%d expected=1, n_samples=%d expected=%d)\n",
        f.n_channels, f.n_samples, FrameSamplesFor(impl_->sample_rate_hz));
    return;
  }

  // Working buffer at 48 kHz.
  std::array<float, kRnnoiseFrameSamples> rnnoise_buf{};

  if (impl_->sample_rate_hz == 48000) {
    // Direct: int16 -> float (int16-range, NOT normalized).
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      rnnoise_buf[i] = static_cast<float>(f.ch[0][i]);
    }
    rnnoise_process_frame(impl_->st, rnnoise_buf.data(), rnnoise_buf.data());
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      const float v = rnnoise_buf[i];
      f.ch[0][i] = static_cast<int16_t>(
          v < -32768.0f ? -32768 : v > 32767.0f ? 32767 : v);
    }
    return;
  }

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
  rnnoise_process_frame(impl_->st, rnnoise_buf.data(), rnnoise_buf.data());
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

}  // namespace ecnr

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

RnNsAdapter::RnNsAdapter() = default;

RnNsAdapter::~RnNsAdapter() {
  if (st_) rnnoise_destroy(st_);
  if (up_) speex_resampler_destroy(up_);
  if (down_) speex_resampler_destroy(down_);
}

bool RnNsAdapter::Init(int sample_rate_hz) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;
  // Tear down any previous state (Reset path or repeated Init).
  if (st_) { rnnoise_destroy(st_); st_ = nullptr; }
  if (up_) { speex_resampler_destroy(up_); up_ = nullptr; }
  if (down_) { speex_resampler_destroy(down_); down_ = nullptr; }

  st_ = rnnoise_create(/*model=*/nullptr);
  if (!st_) return false;

  if (sample_rate_hz == 16000) {
    int err = 0;
    up_ = speex_resampler_init(/*channels=*/1, /*in=*/16000, /*out=*/48000,
                               kSpeexQuality, &err);
    if (!up_ || err != 0) return false;
    down_ = speex_resampler_init(1, 48000, 16000, kSpeexQuality, &err);
    if (!down_ || err != 0) return false;
  }
  // 48 kHz tier: no resampling; up_/down_ stay null.

  sample_rate_hz_ = sample_rate_hz;
  return true;
}

void RnNsAdapter::Reset() {
  if (st_) {
    rnnoise_destroy(st_);
    st_ = rnnoise_create(/*model=*/nullptr);
  }
  if (up_) speex_resampler_reset_mem(up_);
  if (down_) speex_resampler_reset_mem(down_);
}

void RnNsAdapter::Process(Frame& f) {
  if (!st_) return;
  if (f.n_channels != 1 || f.n_samples != FrameSamplesFor(sample_rate_hz_)) {
    std::fprintf(stderr,
        "RnNsAdapter::Process: dropping frame (n_channels=%d expected=1, n_samples=%d expected=%d)\n",
        f.n_channels, f.n_samples, FrameSamplesFor(sample_rate_hz_));
    return;
  }

  // Working buffer at 48 kHz.
  std::array<float, kRnnoiseFrameSamples> rnnoise_buf{};

  if (sample_rate_hz_ == 48000) {
    // Direct: int16 -> float (int16-range, NOT normalized).
    for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
      rnnoise_buf[i] = static_cast<float>(f.ch[0][i]);
    }
    rnnoise_process_frame(st_, rnnoise_buf.data(), rnnoise_buf.data());
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
  speex_resampler_process_int(up_, /*channel=*/0,
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
  rnnoise_process_frame(st_, rnnoise_buf.data(), rnnoise_buf.data());
  std::array<int16_t, kRnnoiseFrameSamples> processed_48k{};
  for (int i = 0; i < kRnnoiseFrameSamples; ++i) {
    const float v = rnnoise_buf[i];
    processed_48k[i] = static_cast<int16_t>(
        v < -32768.0f ? -32768 : v > 32767.0f ? 32767 : v);
  }

  // Step 3: int16 48k -> int16 16k (back to f.ch[0]).
  in_len = kRnnoiseFrameSamples;
  out_len = static_cast<spx_uint32_t>(f.n_samples);
  speex_resampler_process_int(down_, 0,
                              processed_48k.data(), &in_len,
                              f.ch[0].data(), &out_len);
  // Same warmup caveat for downsample. Zero-pad short output.
  for (spx_uint32_t i = out_len; i < static_cast<spx_uint32_t>(f.n_samples); ++i) {
    f.ch[0][i] = 0;
  }
}

}  // namespace ecnr

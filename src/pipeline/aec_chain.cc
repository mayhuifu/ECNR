#include "pipeline/aec_chain.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>

#include "core/frame.h"
#include "pipeline/aec3_adapter.h"
#include "pipeline/beamformer.h"
#include "pipeline/rnnoise_adapter.h"

namespace ecnr {

struct AecChain::Impl {
  Beamformer beamformer;
  Aec3Adapter aec3;
  RnNsAdapter ns;
  ChainStats stats;
  int sample_rate_hz = 0;
  int num_mics = 0;
  int stream_delay_ms = 0;
};

AecChain::AecChain() : impl_(std::make_unique<Impl>()) {}
AecChain::~AecChain() = default;

bool AecChain::Init(int sample_rate_hz, int num_mics) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;
  if (!IsSupportedMicCount(num_mics)) return false;
  if (!impl_->beamformer.Init(sample_rate_hz, num_mics)) return false;
  if (!impl_->aec3.Init(sample_rate_hz)) return false;
  if (!impl_->ns.Init(sample_rate_hz)) return false;
  impl_->sample_rate_hz = sample_rate_hz;
  impl_->num_mics = num_mics;
  Reset();
  return true;
}

void AecChain::ProcessRender(const Frame& render) {
  // Render is mono and must match the configured rate. Drop misshaped frames
  // in non-debug builds; assert in debug.
  assert(render.n_channels == 1);
  assert(render.n_samples == FrameSamplesFor(impl_->sample_rate_hz));
  if (render.n_channels != 1 ||
      render.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    std::fprintf(stderr,
        "AecChain::ProcessRender: dropping frame (n_channels=%d expected=1, n_samples=%d expected=%d)\n",
        render.n_channels, render.n_samples,
        FrameSamplesFor(impl_->sample_rate_hz));
    ++impl_->stats.frames_dropped;
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();
  impl_->aec3.ProcessRender(render);
  const auto t1 = std::chrono::steady_clock::now();
  impl_->stats.cpu_time_s +=
      std::chrono::duration<double>(t1 - t0).count();
}

void AecChain::ProcessCapture(const Frame& mic_in, Frame& uplink_out) {
  assert(mic_in.n_channels == impl_->num_mics);
  assert(mic_in.n_samples == FrameSamplesFor(impl_->sample_rate_hz));
  if (mic_in.n_channels != impl_->num_mics ||
      mic_in.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    std::fprintf(stderr,
        "AecChain::ProcessCapture: dropping frame (n_channels=%d expected=%d, n_samples=%d expected=%d)\n",
        mic_in.n_channels, impl_->num_mics, mic_in.n_samples,
        FrameSamplesFor(impl_->sample_rate_hz));
    ++impl_->stats.frames_dropped;
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();

  // Forward the latest stream-delay seed before processing so AEC3's delay
  // estimator has the freshest hint for this capture frame.
  impl_->aec3.SetStreamDelayMs(impl_->stream_delay_ms);

  Frame post_bf;
  impl_->beamformer.Process(mic_in, post_bf);

  Frame post_aec;
  impl_->aec3.ProcessCapture(post_bf, post_aec);
  impl_->ns.Process(post_aec);

  uplink_out.n_channels = 1;
  uplink_out.n_samples = post_aec.n_samples;
  for (int i = 0; i < post_aec.n_samples; ++i) {
    uplink_out.ch[0][i] = post_aec.ch[0][i];
  }

  const auto t1 = std::chrono::steady_clock::now();
  impl_->stats.cpu_time_s +=
      std::chrono::duration<double>(t1 - t0).count();
  impl_->stats.audio_time_s +=
      static_cast<double>(kFrameDurationMs) / 1000.0;

  // Pull APM-internal stats and copy field-for-field into ChainStats. Per
  // ADR-0006, ChainStats's optional fields mirror webrtc::AudioProcessingStats.
  // APM populates these gradually (delay metrics aggregate over ~1 s windows).
  const Aec3Adapter::Stats a = impl_->aec3.GetStats();
  impl_->stats.echo_return_loss_enhancement_db     = a.echo_return_loss_enhancement_db;
  impl_->stats.echo_return_loss_db                 = a.echo_return_loss_db;
  impl_->stats.residual_echo_likelihood            = a.residual_echo_likelihood;
  impl_->stats.residual_echo_likelihood_recent_max = a.residual_echo_likelihood_recent_max;
  impl_->stats.delay_ms                            = a.delay_ms;
  impl_->stats.delay_median_ms                     = a.delay_median_ms;
  impl_->stats.divergent_filter_fraction           = a.divergent_filter_fraction;
}

void AecChain::Reset() {
  impl_->beamformer.Reset();
  impl_->aec3.Reset();
  impl_->ns.Reset();
  impl_->stats = {};
}

void AecChain::SetStreamDelayMs(int delay_ms) {
  impl_->stream_delay_ms = std::clamp(delay_ms, 0, kMaxStreamDelayMs);
}

void AecChain::SetNsDryBlend(float blend) {
  impl_->ns.SetDryBlend(blend);
}

const ChainStats& AecChain::Stats() const { return impl_->stats; }

}  // namespace ecnr

#include "pipeline/aec3_adapter.h"

#include <array>

#include "api/audio/audio_processing.h"
#include "api/audio/audio_processing_statistics.h"
#include "api/scoped_refptr.h"

#include "core/frame.h"

namespace ecnr {

struct Aec3Adapter::Impl {
  // APM is reference-counted (Builder::Create() returns rtc::scoped_refptr).
  rtc::scoped_refptr<webrtc::AudioProcessing> apm;
  int sample_rate_hz = 0;
  // Scratch buffer for ProcessReverseStream's `dest` parameter — APM requires
  // a writable destination even when we don't consume the post-process render.
  // Sized for the largest supported rate (48 kHz, mono, 10 ms = 480 samples).
  std::array<int16_t, kFrameSamples48k> reverse_scratch{};
};

Aec3Adapter::Aec3Adapter() : impl_(std::make_unique<Impl>()) {}
Aec3Adapter::~Aec3Adapter() = default;

bool Aec3Adapter::Init(int sample_rate_hz) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;

  webrtc::AudioProcessingBuilder builder;
  impl_->apm = builder.Create();
  if (!impl_->apm) return false;

  webrtc::AudioProcessing::Config cfg;
  cfg.echo_canceller.enabled = true;
  cfg.echo_canceller.mobile_mode = false;
  cfg.high_pass_filter.enabled = true;
  cfg.noise_suppression.enabled = false;   // RNNoise (Task 7)
  cfg.gain_controller1.enabled = false;    // AGC out of Phase 0.5 scope
  cfg.gain_controller2.enabled = false;
  impl_->apm->ApplyConfig(cfg);

  impl_->sample_rate_hz = sample_rate_hz;
  return true;
}

void Aec3Adapter::Reset() {
  if (!impl_->apm) return;
  // Re-prepare internal state without reconstructing the APM instance.
  impl_->apm->Initialize();
}

void Aec3Adapter::SetStreamDelayMs(int ms) {
  if (!impl_->apm) return;
  impl_->apm->set_stream_delay_ms(ms);
}

void Aec3Adapter::ProcessRender(const Frame& render) {
  if (!impl_->apm) return;
  if (render.n_channels != 1 ||
      render.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    return;  // Internal safety net; chain's frames_dropped is the contract.
  }
  webrtc::StreamConfig sc(impl_->sample_rate_hz, /*num_channels=*/1);
  impl_->apm->ProcessReverseStream(
      render.ch[0].data(), sc, sc, impl_->reverse_scratch.data());
}

void Aec3Adapter::ProcessCapture(const Frame& capture, Frame& out) {
  if (!impl_->apm) {
    out = capture;
    return;
  }
  if (capture.n_channels != 1 ||
      capture.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    out = capture;
    return;
  }
  out.n_channels = 1;
  out.n_samples = capture.n_samples;
  webrtc::StreamConfig sc(impl_->sample_rate_hz, /*num_channels=*/1);
  impl_->apm->ProcessStream(
      capture.ch[0].data(), sc, sc, out.ch[0].data());
}

Aec3Adapter::Stats Aec3Adapter::GetStats() const {
  Stats s;
  if (!impl_->apm) return s;
  const webrtc::AudioProcessingStats a = impl_->apm->GetStatistics();
  s.echo_return_loss_enhancement_db     = a.echo_return_loss_enhancement;
  s.echo_return_loss_db                 = a.echo_return_loss;
  s.residual_echo_likelihood            = a.residual_echo_likelihood;
  s.residual_echo_likelihood_recent_max = a.residual_echo_likelihood_recent_max;
  s.delay_ms                            = a.delay_ms;
  s.delay_median_ms                     = a.delay_median_ms;
  s.divergent_filter_fraction           = a.divergent_filter_fraction;
  return s;
}

}  // namespace ecnr

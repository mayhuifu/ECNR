#include "pipeline/agc2_adapter.h"

#include <array>

#include "api/audio/audio_processing.h"
#include "api/scoped_refptr.h"

#include "core/frame.h"

namespace ecnr {

struct Agc2Adapter::Impl {
  rtc::scoped_refptr<webrtc::AudioProcessing> apm;
  int sample_rate_hz = 0;
};

Agc2Adapter::Agc2Adapter() : impl_(std::make_unique<Impl>()) {}
Agc2Adapter::~Agc2Adapter() = default;

bool Agc2Adapter::Init(int sample_rate_hz) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;

  webrtc::AudioProcessingBuilder builder;
  impl_->apm = builder.Create();
  if (!impl_->apm) return false;

  webrtc::AudioProcessing::Config cfg;
  // AGC2-only: every other APM stage off so this APM does nothing but
  // adaptive-digital gain. AEC + NS + HPF all live in their own stages
  // upstream of this adapter (see AecChain wiring).
  cfg.echo_canceller.enabled       = false;
  cfg.high_pass_filter.enabled     = false;
  cfg.noise_suppression.enabled    = false;
  cfg.gain_controller1.enabled                  = false;
  cfg.gain_controller2.enabled                  = true;
  // gain_controller2.enabled alone is NOT enough — the sub-flag
  // adaptive_digital.enabled is what actually wires the digital AGC.
  // The defaults (max_gain_db=50, initial_gain_db=15, headroom_db=5,
  // max_gain_change=6 dB/s) target ~-23 dBFS final output, which is
  // safely inside the 3GPP TS 26.131 hands-free SLR target band once
  // the standard sensitivity mapping is applied. Leaving them at
  // upstream defaults for now; a Phase-2 tuning pass against real
  // cabin recordings can adjust if measurements indicate the target
  // level needs shifting.
  cfg.gain_controller2.adaptive_digital.enabled = true;
  cfg.gain_controller2.input_volume_controller.enabled = false;
  impl_->apm->ApplyConfig(cfg);

  impl_->sample_rate_hz = sample_rate_hz;
  return true;
}

void Agc2Adapter::Reset() {
  if (!impl_->apm) return;
  impl_->apm->Initialize();
}

void Agc2Adapter::Process(Frame& f) {
  if (!impl_->apm) return;
  if (f.n_channels != 1 ||
      f.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    return;  // Internal safety net; chain enforces shape upstream.
  }
  webrtc::StreamConfig sc(impl_->sample_rate_hz, /*num_channels=*/1);
  // In-place: APM accepts identical src/dest pointers (the implementation
  // copies internally as needed).
  impl_->apm->ProcessStream(f.ch[0].data(), sc, sc, f.ch[0].data());
}

Agc2Adapter::Stats Agc2Adapter::GetStats() const {
  Stats s;
  if (!impl_->apm) return s;
  // The WebRTC AudioProcessingStats struct doesn't expose AGC2 applied
  // gain directly (it's part of internal state). For now we report
  // nullopt; if the upstream API gains a getter we'll wire it. The
  // bench can still verify AGC2 is working by measuring output RMS
  // before/after a flag flip.
  return s;
}

}  // namespace ecnr

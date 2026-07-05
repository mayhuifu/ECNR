#include "pipeline/aec3_adapter.h"

#include <algorithm>
#include <array>

#include "api/audio/audio_processing.h"
#include "api/audio/audio_processing_statistics.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_control.h"
#include "api/scoped_refptr.h"
// EchoCanceller3 itself is not part of the installed webrtc-audio-processing
// API surface, but its object code is in the static archive and its header
// ships in the vendored source tree (pinned by vendor/MANIFEST.tsv). We
// include it from the source tree to build a tuned EchoControlFactory —
// the only sanctioned hook (AudioProcessingBuilder::SetEchoControlFactory)
// for injecting a non-default EchoCanceller3Config. Private-API risk is
// bounded by the vendor pin; revisit on any upstream bump.
#include "modules/audio_processing/aec3/echo_canceller3.h"

#include "core/frame.h"

namespace ecnr {

namespace {

// EchoControlFactory that constructs EchoCanceller3 with an explicit config
// instead of the WebRTC defaults. Used only when SetFilterLengthBlocks(>0)
// was called before Init; otherwise APM's built-in default factory runs.
class TunedAec3Factory : public webrtc::EchoControlFactory {
 public:
  explicit TunedAec3Factory(const webrtc::EchoCanceller3Config& cfg)
      : cfg_(cfg) {}
  std::unique_ptr<webrtc::EchoControl> Create(
      int sample_rate_hz, int num_render_channels,
      int num_capture_channels) override {
    return std::make_unique<webrtc::EchoCanceller3>(
        cfg_, /*multichannel_config=*/std::nullopt, sample_rate_hz,
        static_cast<size_t>(num_render_channels),
        static_cast<size_t>(num_capture_channels));
  }

 private:
  webrtc::EchoCanceller3Config cfg_;
};

}  // namespace

struct Aec3Adapter::Impl {
  // APM is reference-counted (Builder::Create() returns rtc::scoped_refptr).
  rtc::scoped_refptr<webrtc::AudioProcessing> apm;
  int sample_rate_hz = 0;
  // 0 = WebRTC default filter lengths (13 blocks). See SetFilterLengthBlocks.
  int filter_length_blocks = 0;
  // All-sentinel = WebRTC default suppressor behaviour. See SetDtTuning.
  AecDtTuning dt_tuning;
  // Scratch buffer for ProcessReverseStream's `dest` parameter — APM requires
  // a writable destination even when we don't consume the post-process render.
  // Sized for the largest supported rate (48 kHz, mono, 10 ms = 480 samples).
  std::array<int16_t, kFrameSamples48k> reverse_scratch{};
};

Aec3Adapter::Aec3Adapter() : impl_(std::make_unique<Impl>()) {}
Aec3Adapter::~Aec3Adapter() = default;

void Aec3Adapter::SetFilterLengthBlocks(int blocks) {
  // Stored for the next Init(); clamped to a sane band. Lower bound 9:
  // below that, RenderDelayBuffer's size derivation from
  // filter.refined.length_blocks (render_delay_buffer.cc:135) underflows
  // and APM construction dies with std::length_error (measured 2026-07-04:
  // 9 runs, 8 crashes). Upper 20 = beyond-default headroom.
  impl_->filter_length_blocks =
      blocks <= 0 ? 0 : std::clamp(blocks, 9, 20);
}

void Aec3Adapter::SetDtTuning(const AecDtTuning& tuning) {
  // Stored for the next Init(); Validate() sanitizes structural nonsense
  // (e.g. transparent > suppress mask inversions) at config-build time.
  impl_->dt_tuning = tuning;
}

bool Aec3Adapter::Init(int sample_rate_hz) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;

  webrtc::AudioProcessingBuilder builder;
  const AecDtTuning& dt = impl_->dt_tuning;
  if (impl_->filter_length_blocks > 0 || dt.Any()) {
    webrtc::EchoCanceller3Config cfg3;
    if (impl_->filter_length_blocks > 0) {
      const size_t n = static_cast<size_t>(impl_->filter_length_blocks);
      cfg3.filter.refined.length_blocks = n;
      cfg3.filter.coarse.length_blocks = n;
      // Initial-convergence filters default to 12 blocks; they must not
      // exceed the steady-state filters.
      cfg3.filter.refined_initial.length_blocks =
          std::min(cfg3.filter.refined_initial.length_blocks, n);
      cfg3.filter.coarse_initial.length_blocks =
          std::min(cfg3.filter.coarse_initial.length_blocks, n);
    }
    // Double-talk transparency overrides (GB/T 45314 §5.7, aec_tuning.h).
    auto& sup = cfg3.suppressor;
    if (dt.nearend_enr_threshold >= 0.0f)
      sup.dominant_nearend_detection.enr_threshold = dt.nearend_enr_threshold;
    if (dt.nearend_snr_threshold >= 0.0f)
      sup.dominant_nearend_detection.snr_threshold = dt.nearend_snr_threshold;
    if (dt.nearend_hold_duration >= 0)
      sup.dominant_nearend_detection.hold_duration = dt.nearend_hold_duration;
    if (dt.nearend_trigger_threshold >= 0)
      sup.dominant_nearend_detection.trigger_threshold =
          dt.nearend_trigger_threshold;
    if (dt.mask_lf_enr_transparent >= 0.0f)
      sup.nearend_tuning.mask_lf.enr_transparent = dt.mask_lf_enr_transparent;
    if (dt.mask_lf_enr_suppress >= 0.0f)
      sup.nearend_tuning.mask_lf.enr_suppress = dt.mask_lf_enr_suppress;
    if (dt.mask_hf_enr_transparent >= 0.0f)
      sup.nearend_tuning.mask_hf.enr_transparent = dt.mask_hf_enr_transparent;
    if (dt.mask_hf_enr_suppress >= 0.0f)
      sup.nearend_tuning.mask_hf.enr_suppress = dt.mask_hf_enr_suppress;
    if (dt.max_dec_factor_lf >= 0.0f)
      sup.nearend_tuning.max_dec_factor_lf = dt.max_dec_factor_lf;
    // Upstream sanitizer: clamps anything structurally invalid in place.
    webrtc::EchoCanceller3Config::Validate(&cfg3);
    builder.SetEchoControlFactory(std::make_unique<TunedAec3Factory>(cfg3));
  }
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

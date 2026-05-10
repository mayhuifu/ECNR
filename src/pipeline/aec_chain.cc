#include "pipeline/aec_chain.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "pipeline/beamformer.h"

namespace ecnr {

namespace {

// Phase 0 stub: a primitive linear AEC that subtracts an exponentially-smoothed
// estimate of the render frame from capture, with a fixed delay-line. NOT a
// real AEC — present only to exercise the chain plumbing and produce a non-zero
// ERLE for the smoke test. Phase 0.5 replaces with webrtc::AudioProcessing.
//
// Mono-only by design: the Beamformer upstream collapses N→1 before this stub
// sees the frame, so we always operate on ch[0].
class StubLinearAec {
 public:
  void ProcessRender(const Frame& render) {
    // Push to a 16-frame delay line so capture sees correlated history.
    if (render_history_.size() >= 16) render_history_.erase(render_history_.begin());
    render_history_.push_back(render);
  }

  // out = capture - alpha * (most recent render). Phase 0 only. Operates on
  // ch[0] only; assumes the caller already collapsed N→1 via Beamformer.
  void ProcessCapture(const Frame& capture, Frame& out) {
    out.n_channels = 1;
    out.n_samples = capture.n_samples;
    if (render_history_.empty()) {
      for (int i = 0; i < capture.n_samples; ++i) {
        out.ch[0][i] = capture.ch[0][i];
      }
      return;
    }
    const Frame& ref = render_history_.back();
    constexpr double kAlpha = 0.7;
    for (int i = 0; i < capture.n_samples; ++i) {
      const int32_t y =
          static_cast<int32_t>(capture.ch[0][i]) -
          static_cast<int32_t>(std::round(kAlpha * ref.ch[0][i]));
      out.ch[0][i] = static_cast<int16_t>(std::clamp(y, -32768, 32767));
    }
  }

  void Reset() { render_history_.clear(); }

 private:
  std::vector<Frame> render_history_;
};

// Phase 0 stub NS: noop pass-through. Phase 0.5 replaces with rnnoise_process_frame.
class StubNs {
 public:
  void Process(Frame& f) { (void)f; }
  void Reset() {}
};

}  // namespace

struct AecChain::Impl {
  Beamformer beamformer;
  StubLinearAec aec;
  StubNs ns;
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
  if (render.n_channels != 1) return;
  if (render.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) return;

  const auto t0 = std::chrono::steady_clock::now();
  impl_->aec.ProcessRender(render);
  const auto t1 = std::chrono::steady_clock::now();
  impl_->stats.cpu_time_s +=
      std::chrono::duration<double>(t1 - t0).count();
}

void AecChain::ProcessCapture(const Frame& mic_in, Frame& uplink_out) {
  assert(mic_in.n_channels == impl_->num_mics);
  assert(mic_in.n_samples == FrameSamplesFor(impl_->sample_rate_hz));
  if (mic_in.n_channels != impl_->num_mics) return;
  if (mic_in.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) return;

  const auto t0 = std::chrono::steady_clock::now();

  Frame post_bf;
  impl_->beamformer.Process(mic_in, post_bf);

  Frame post_aec;
  impl_->aec.ProcessCapture(post_bf, post_aec);
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

  // Per ADR-0006: APM-style stats (ERLE, ERL, residual-echo likelihood, ...)
  // are populated by the WebRTC backend wired in Task 6. Under the stub,
  // all optional fields stay nullopt — a manually-computed energy ratio
  // here would be misleading and is intentionally not surfaced.
}

void AecChain::Reset() {
  impl_->beamformer.Reset();
  impl_->aec.Reset();
  impl_->ns.Reset();
  impl_->stats = {};
}

void AecChain::SetStreamDelayMs(int delay_ms) {
  impl_->stream_delay_ms = std::clamp(delay_ms, 0, kMaxStreamDelayMs);
}

const ChainStats& AecChain::Stats() const { return impl_->stats; }

}  // namespace ecnr

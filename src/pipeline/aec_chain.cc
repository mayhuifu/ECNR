#include "pipeline/aec_chain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/frame.h"

namespace ecnr {

namespace {

// Energy of an int16 frame, normalized to [0, 1].
double FrameEnergy(const Frame& f) {
  double e = 0.0;
  for (int16_t s : f.samples) {
    const double x = static_cast<double>(s) / 32768.0;
    e += x * x;
  }
  return e / kFrameSamples;
}

// Phase 0 stub: a primitive linear AEC that subtracts an exponentially-smoothed
// estimate of the render frame from capture, with a fixed delay-line. NOT a
// real AEC — present only to exercise the chain plumbing and produce a non-zero
// ERLE for the smoke test. Phase 0.5 replaces with webrtc::AudioProcessing.
class StubLinearAec {
 public:
  void ProcessRender(const Frame& render) {
    // Push to a 16-frame delay line so capture sees correlated history.
    if (render_history_.size() >= 16) render_history_.erase(render_history_.begin());
    render_history_.push_back(render);
  }

  // out = capture - alpha * (most recent render). Phase 0 only.
  void ProcessCapture(const Frame& capture, Frame& out) {
    if (render_history_.empty()) {
      out = capture;
      return;
    }
    const Frame& ref = render_history_.back();
    constexpr double kAlpha = 0.7;
    for (int i = 0; i < kFrameSamples; ++i) {
      const int32_t y =
          static_cast<int32_t>(capture.samples[i]) -
          static_cast<int32_t>(std::round(kAlpha * ref.samples[i]));
      out.samples[i] = static_cast<int16_t>(std::clamp(y, -32768, 32767));
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
  StubLinearAec aec;
  StubNs ns;
  ChainStats stats;
  int sample_rate_hz = 0;
};

AecChain::AecChain() : impl_(std::make_unique<Impl>()) {}
AecChain::~AecChain() = default;

bool AecChain::Init(int sample_rate_hz) {
  if (sample_rate_hz != kSampleRateHz) return false;
  impl_->sample_rate_hz = sample_rate_hz;
  Reset();
  return true;
}

void AecChain::ProcessRender(const Frame& render) {
  const auto t0 = std::chrono::steady_clock::now();
  impl_->aec.ProcessRender(render);
  const auto t1 = std::chrono::steady_clock::now();
  impl_->stats.cpu_time_s +=
      std::chrono::duration<double>(t1 - t0).count();
}

void AecChain::ProcessCapture(const Frame& capture, Frame& out) {
  const auto t0 = std::chrono::steady_clock::now();

  const double e_in = FrameEnergy(capture);
  impl_->aec.ProcessCapture(capture, out);
  impl_->ns.Process(out);
  const double e_out = FrameEnergy(out);

  const auto t1 = std::chrono::steady_clock::now();
  impl_->stats.cpu_time_s +=
      std::chrono::duration<double>(t1 - t0).count();
  impl_->stats.audio_time_s +=
      static_cast<double>(kFrameDurationMs) / 1000.0;

  // ERLE = 10 log10(E_in / E_out), clamped to non-negative for sanity.
  if (e_in > 1e-12 && e_out > 1e-12) {
    impl_->stats.erle_db = 10.0 * std::log10(e_in / e_out);
  } else {
    impl_->stats.erle_db = 0.0;
  }
}

void AecChain::Reset() {
  impl_->aec.Reset();
  impl_->ns.Reset();
  impl_->stats = {};
}

const ChainStats& AecChain::Stats() const { return impl_->stats; }

}  // namespace ecnr

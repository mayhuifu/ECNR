#include "pipeline/aec_chain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/frame.h"

namespace ecnr {

namespace {

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
  int stream_delay_ms = 0;
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

  impl_->aec.ProcessCapture(capture, out);
  impl_->ns.Process(out);

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
  impl_->aec.Reset();
  impl_->ns.Reset();
  impl_->stats = {};
}

bool AecChain::SetStreamDelayMs(int ms) {
  if (ms < 0 || ms > 500) return false;
  impl_->stream_delay_ms = ms;
  return true;
}

const ChainStats& AecChain::Stats() const { return impl_->stats; }

}  // namespace ecnr

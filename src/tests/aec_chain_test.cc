#include "pipeline/aec_chain.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>

#include "core/frame.h"

namespace ecnr {
namespace {

// Fills `f` with white noise scaled to roughly half-scale int16.
void FillNoise(Frame& f, std::mt19937& rng) {
  std::uniform_int_distribution<int> dist(-16384, 16384);
  for (auto& s : f.samples) s = static_cast<int16_t>(dist(rng));
}

// Mixes far-end echo into a clean near-end frame: out = near + alpha * far.
void MixEcho(const Frame& near, const Frame& far, double alpha, Frame& out) {
  for (int i = 0; i < kFrameSamples; ++i) {
    const int32_t y =
        static_cast<int32_t>(near.samples[i]) +
        static_cast<int32_t>(std::round(alpha * far.samples[i]));
    out.samples[i] = static_cast<int16_t>(std::clamp(y, -32768, 32767));
  }
}

TEST(AecChain, InitRejectsWrongSampleRate) {
  AecChain c;
  EXPECT_FALSE(c.Init(8000));
  EXPECT_FALSE(c.Init(48000));
  EXPECT_TRUE(c.Init(kSampleRateHz));
}

// Smoke test: feed correlated render + capture (capture = render echoed) and
// expect the chain to attenuate the capture energy. Phase 0 stub uses a simple
// alpha-subtraction so we only assert that *some* attenuation happens — Phase
// 0.5 will tighten this to ERLE > 10 dB once AEC3 lands.
TEST(AecChain, AttenuatesCorrelatedEcho) {
  AecChain c;
  ASSERT_TRUE(c.Init(kSampleRateHz));

  std::mt19937 rng(0x1234);
  Frame zero_near{};
  Frame far, mic, out;
  // Echo gain != stub alpha (0.7) so the stub leaves a measurable residual
  // rather than perfectly cancelling.
  constexpr double kEchoGain = 0.5;

  // Run 200 frames (2 seconds). Stub AEC needs a frame of warmup before the
  // delay line is populated, so check ERLE on later frames only.
  double cumulative_in_e = 0.0;
  double cumulative_out_e = 0.0;
  for (int i = 0; i < 200; ++i) {
    FillNoise(far, rng);
    MixEcho(zero_near, far, kEchoGain, mic);
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
    if (i >= 50) {
      for (int j = 0; j < kFrameSamples; ++j) {
        const double xin = static_cast<double>(mic.samples[j]) / 32768.0;
        const double xout = static_cast<double>(out.samples[j]) / 32768.0;
        cumulative_in_e += xin * xin;
        cumulative_out_e += xout * xout;
      }
    }
  }

  ASSERT_GT(cumulative_in_e, 0.0);
  ASSERT_GT(cumulative_out_e, 0.0);
  const double erle_db = 10.0 * std::log10(cumulative_in_e / cumulative_out_e);
  // Stub AEC threshold: at least 5 dB. Phase 0.5 will raise to 15 dB once
  // AEC3 + RNNoise replace the stub.
  EXPECT_GT(erle_db, 5.0) << "Phase 0 stub AEC failed to attenuate echo";
}

TEST(AecChain, RtfIsMeasured) {
  AecChain c;
  ASSERT_TRUE(c.Init(kSampleRateHz));
  std::mt19937 rng(0x5678);
  Frame far, mic, out;
  for (int i = 0; i < 10; ++i) {
    FillNoise(far, rng);
    FillNoise(mic, rng);
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
  }
  const auto& s = c.Stats();
  EXPECT_GT(s.audio_time_s, 0.099);  // ~10 frames * 10ms
  EXPECT_LT(s.audio_time_s, 0.101);
  EXPECT_GE(s.cpu_time_s, 0.0);
  EXPECT_LT(s.Rtf(), 1.0) << "stub chain should be far faster than realtime";
}

}  // namespace
}  // namespace ecnr

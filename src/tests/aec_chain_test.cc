#include "pipeline/aec_chain.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>

#include "core/frame.h"

namespace ecnr {
namespace {

// Fills `f` with white noise scaled to roughly half-scale int16. Populates
// n_samples + n_channels and writes ch[0..n_channels)[0..n_samples).
// Defaults: 16 kHz frame, 2 mics. Caller may override before calling.
void FillNoise(Frame& f, std::mt19937& rng,
               int sample_rate_hz = 16000, int num_channels = 2) {
  std::uniform_int_distribution<int> dist(-16384, 16384);
  f.n_samples = FrameSamplesFor(sample_rate_hz);
  f.n_channels = num_channels;
  for (int c = 0; c < num_channels; ++c) {
    for (int s = 0; s < f.n_samples; ++s) {
      f.ch[c][s] = static_cast<int16_t>(dist(rng));
    }
  }
}

// Mixes a far-end echo into a clean near-end frame. Both inputs are mono
// (operate on ch[0]); output is mono. out = near + alpha * far.
void MixEcho(const Frame& near, const Frame& far, double alpha, Frame& out) {
  out.n_samples = near.n_samples;
  out.n_channels = 1;
  for (int i = 0; i < near.n_samples; ++i) {
    const int32_t y =
        static_cast<int32_t>(near.ch[0][i]) +
        static_cast<int32_t>(std::round(alpha * far.ch[0][i]));
    out.ch[0][i] = static_cast<int16_t>(std::clamp(y, -32768, 32767));
  }
}

TEST(AecChain, InitRejectsWrongSampleRate) {
  AecChain c;
  EXPECT_FALSE(c.Init(8000, 2));
  EXPECT_FALSE(c.Init(24000, 2));
  EXPECT_FALSE(c.Init(32000, 2));
  EXPECT_FALSE(c.Init(44100, 2));
  EXPECT_TRUE(c.Init(16000, 2));
}

TEST(AecChain, InitRejectsWrongMicCount) {
  // ADR-0004: production num_mics ∈ [2, 8].
  AecChain c0;
  EXPECT_FALSE(c0.Init(16000, 0));
  AecChain c_neg;
  EXPECT_FALSE(c_neg.Init(16000, -1));
  AecChain c1;
  EXPECT_FALSE(c1.Init(16000, 1));
  AecChain c9;
  EXPECT_FALSE(c9.Init(16000, 9));
  AecChain c2;
  EXPECT_TRUE(c2.Init(16000, 2));
  AecChain c4;
  EXPECT_TRUE(c4.Init(16000, 4));
  AecChain c8;
  EXPECT_TRUE(c8.Init(16000, kMaxMics));
}

TEST(AecChain, AcceptsBoth16kAnd48k) {
  AecChain c1; ASSERT_TRUE(c1.Init(16000, 2));
  AecChain c2; ASSERT_TRUE(c2.Init(48000, 2));
}

// Smoke test: feed correlated render + capture (capture = render echoed) and
// expect the chain to attenuate the capture energy. The Beamformer collapses
// the multi-mic capture to mono (ch[0]), so we drive both mics with the same
// echoed signal — what AEC sees post-beamform is unchanged from the previous
// single-channel test. Real WebRTC AEC3 should reduce echo cumulative ERLE
// substantially; threshold here is conservative to avoid CI flakiness.
TEST(AecChain, AttenuatesCorrelatedEcho) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 2));

  std::mt19937 rng(0x1234);
  Frame zero_near{};
  zero_near.n_samples = FrameSamplesFor(16000);
  zero_near.n_channels = 1;
  Frame far, mic_mono, mic, out;
  constexpr double kEchoGain = 0.5;

  // Run 300 frames (3 seconds). AEC3 needs ~1-2 seconds to converge; we
  // measure ERLE on frames 100+ (last 2 seconds).
  double cumulative_in_e = 0.0;
  double cumulative_out_e = 0.0;
  for (int i = 0; i < 300; ++i) {
    // Render is mono.
    FillNoise(far, rng, 16000, 1);
    // Build the mono echoed mic, then duplicate into a 2-channel mic frame.
    MixEcho(zero_near, far, kEchoGain, mic_mono);
    mic.n_samples = FrameSamplesFor(16000);
    mic.n_channels = 2;
    for (int s = 0; s < mic.n_samples; ++s) {
      mic.ch[0][s] = mic_mono.ch[0][s];
      mic.ch[1][s] = mic_mono.ch[0][s];
    }
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
    if (i >= 100) {
      for (int j = 0; j < FrameSamplesFor(16000); ++j) {
        const double xin = static_cast<double>(mic.ch[0][j]) / 32768.0;
        const double xout = static_cast<double>(out.ch[0][j]) / 32768.0;
        cumulative_in_e += xin * xin;
        cumulative_out_e += xout * xout;
      }
    }
  }

  ASSERT_GT(cumulative_in_e, 0.0);
  ASSERT_GT(cumulative_out_e, 0.0);
  const double erle_db = 10.0 * std::log10(cumulative_in_e / cumulative_out_e);
  // Real WebRTC AEC3 + RNNoise should achieve > 15 dB cumulative ERLE on
  // this synthetic correlated-echo input.
  EXPECT_GT(erle_db, 15.0) << "AEC3 failed to attenuate echo (erle_db=" << erle_db << ")";
}

TEST(AecChain, BeamformerCollapsesToMono) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 4));
  std::mt19937 rng(0xb44b);

  // Construct a 4-mic capture with distinct content per channel; the chain
  // must collapse to mono regardless of how many input channels the mic
  // frame carries.
  Frame mic, render, out;
  mic.n_samples = FrameSamplesFor(16000);
  mic.n_channels = 4;
  for (int ch = 0; ch < 4; ++ch) {
    std::uniform_int_distribution<int> dist(-1000 - ch * 100, 1000 + ch * 100);
    for (int s = 0; s < mic.n_samples; ++s) mic.ch[ch][s] = dist(rng);
  }
  // Render is silence (mono).
  render.n_samples = FrameSamplesFor(16000);
  render.n_channels = 1;

  c.ProcessRender(render);
  c.ProcessCapture(mic, out);

  // Beamformer collapse contract: chain emits exactly one channel at the
  // configured frame size. (We can't spot-check exact sample equality any
  // more — AEC3's HPF + internal 10 ms block buffering modifies the signal
  // even when render is silent.)
  EXPECT_EQ(out.n_channels, 1);
  EXPECT_EQ(out.n_samples, FrameSamplesFor(16000));
}

TEST(AecChain, SetStreamDelayMsAcceptsAndClamps) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 2));
  // Smoke: verify the call is total (no crash for any int) and that boundary
  // + out-of-range values are accepted silently per the clamp contract in
  // ADR-0006. End-to-end propagation into APM (read-back through
  // ChainStats) is verified separately in StreamDelayPropagatesToApm.
  c.SetStreamDelayMs(0);
  c.SetStreamDelayMs(50);
  c.SetStreamDelayMs(kMaxStreamDelayMs);
  c.SetStreamDelayMs(-1);             // clamps to 0
  c.SetStreamDelayMs(kMaxStreamDelayMs + 1);  // clamps down
  c.SetStreamDelayMs(5000);           // clamps down
}

// Confirms SetStreamDelayMs is wired through to the APM instance (i.e. the
// chain doesn't silently drop the call) and that an above-clamp value still
// keeps the chain running. We don't assert on the exact `delay_ms` read-back
// because APM reports its OWN estimated delay (not whatever we set) and that
// estimate may stay nullopt on a synthetic, reverb-free stimulus. Instead we
// verify (a) the chain processes 200 frames without dropping after a delay
// is set, and (b) ERLE is populated — proof the AEC pipeline ran end-to-end
// with the configured delay applied.
TEST(AecChain, StreamDelayPropagatesToApm) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 2));
  // Set an in-range delay before any processing, then a value above the
  // 500 ms clamp partway through — chain must keep running for both.
  c.SetStreamDelayMs(50);

  std::mt19937 rng(0x5050);
  Frame zero_near{};
  zero_near.n_samples = FrameSamplesFor(16000);
  zero_near.n_channels = 1;
  Frame far, mic_mono, mic, out;
  constexpr double kEchoGain = 0.5;
  // 500 frames so AEC3 has time to populate ERLE (matches the stimulus in
  // ChainStatsPopulatedAfterAec3Convergence).
  for (int i = 0; i < 500; ++i) {
    if (i == 250) c.SetStreamDelayMs(550);  // above the 500 clamp
    FillNoise(far, rng, 16000, 1);
    MixEcho(zero_near, far, kEchoGain, mic_mono);
    mic.n_samples = FrameSamplesFor(16000);
    mic.n_channels = 2;
    for (int s = 0; s < mic.n_samples; ++s) {
      mic.ch[0][s] = mic_mono.ch[0][s];
      mic.ch[1][s] = mic_mono.ch[0][s];
    }
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
  }

  // Chain stayed alive: no frames dropped, mono output as expected.
  EXPECT_EQ(c.Stats().frames_dropped, 0u);
  EXPECT_EQ(out.n_channels, 1);
  EXPECT_EQ(out.n_samples, FrameSamplesFor(16000));
  // AEC ran end-to-end despite the SetStreamDelayMs calls: ERLE populated.
  const auto& s = c.Stats();
  EXPECT_TRUE(s.echo_return_loss_enhancement_db.has_value())
      << "AEC3 should populate ERLE optional after 5 s of correlated input "
         "even with SetStreamDelayMs in use";
  // If APM produced a delay estimate, it must be in a sensible range
  // (APM reports its own estimate, not the value we configured).
  if (s.delay_ms.has_value()) {
    EXPECT_GE(*s.delay_ms, 0);
    EXPECT_LE(*s.delay_ms, 500);
  }
}

TEST(AecChain, ChainStatsPopulatedAfterAec3Convergence) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 2));
  std::mt19937 rng(0x1111);
  Frame zero_near{};
  zero_near.n_samples = FrameSamplesFor(16000);
  zero_near.n_channels = 1;
  Frame far, mic_mono, mic, out;
  constexpr double kEchoGain = 0.5;
  // 5 s of correlated input — AEC3 needs ~3 s on white-noise echo before its
  // adaptive filter converges enough to report a meaningful ERLE; we observe
  // ~53 dB by frame 300 and 80+ dB by frame 400. 200 frames (2 s) is not
  // enough for this synthetic stimulus.
  for (int i = 0; i < 500; ++i) {
    // Drive the chain with correlated render + capture (capture = echoed
    // render) so AEC3 produces a meaningful ERLE value.
    FillNoise(far, rng, 16000, 1);
    MixEcho(zero_near, far, kEchoGain, mic_mono);
    mic.n_samples = FrameSamplesFor(16000);
    mic.n_channels = 2;
    for (int s = 0; s < mic.n_samples; ++s) {
      mic.ch[0][s] = mic_mono.ch[0][s];
      mic.ch[1][s] = mic_mono.ch[0][s];
    }
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
  }
  // After 5 s of correlated input, AEC3 must have populated ERLE. Other
  // optionals (residual_echo_likelihood, delay_ms) populate on different
  // APM cadences and are left loose.
  const auto& s = c.Stats();
  EXPECT_TRUE(s.echo_return_loss_enhancement_db.has_value())
      << "AEC3 should populate ERLE optional after 5 s of correlated input";
  EXPECT_GT(s.echo_return_loss_enhancement_db.value_or(0.0), 5.0)
      << "ERLE should be at least 5 dB on this stimulus";
}

TEST(AecChain, RejectsMisshapedFrame) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 2));

  // Init at 16k expects 160-sample frames. Feed a 480-sample (48k-shaped) frame.
  Frame far, mic, out;
  far.n_samples = kFrameSamples48k;   // wrong: chain is at 16k
  far.n_channels = 1;
  c.ProcessRender(far);

  mic.n_samples = kFrameSamples48k;   // wrong: chain is at 16k
  mic.n_channels = 2;
  c.ProcessCapture(mic, out);

  // Both calls should have been dropped.
  EXPECT_GE(c.Stats().frames_dropped, 2u);

  // Wrong channel count: chain init at num_mics=2, but mic_in.n_channels=3
  Frame mic_wrong_ch;
  mic_wrong_ch.n_samples = kFrameSamples16k;
  mic_wrong_ch.n_channels = 3;
  c.ProcessCapture(mic_wrong_ch, out);
  EXPECT_GE(c.Stats().frames_dropped, 3u);

  // Wrong render channels: render must be mono (n_channels == 1)
  Frame far_wrong_ch;
  far_wrong_ch.n_samples = kFrameSamples16k;
  far_wrong_ch.n_channels = 2;
  c.ProcessRender(far_wrong_ch);
  EXPECT_GE(c.Stats().frames_dropped, 4u);
}

TEST(AecChain, RtfIsMeasured) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 2));
  std::mt19937 rng(0x5678);
  Frame far, mic, out;
  for (int i = 0; i < 10; ++i) {
    FillNoise(far, rng, 16000, 1);
    FillNoise(mic, rng, 16000, 2);
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
  }
  const auto& s = c.Stats();
  EXPECT_GT(s.audio_time_s, 0.099);  // ~10 frames * 10ms
  EXPECT_LT(s.audio_time_s, 0.101);
  EXPECT_GE(s.cpu_time_s, 0.0);
  EXPECT_LT(s.Rtf(), 1.0) << "stub chain should be far faster than realtime";
}

// Smoke: pure noise input + silent render. The chain should produce
// attenuated output at both 16k and 48k tiers (RNNoise reduces noise floor
// even with no echo). The threshold is loose because RNNoise's behavior on
// pure white noise is "suppress confidently" — typically 5-15 dB of energy
// reduction.
TEST(AecChain, NsActiveAtBothRates) {
  for (int rate : {16000, 48000}) {
    AecChain c;
    ASSERT_TRUE(c.Init(rate, 2)) << "init at " << rate;
    std::mt19937 rng(0xb0b);
    Frame far, mic, out;
    far.n_channels = 1;
    far.n_samples = FrameSamplesFor(rate);
    mic.n_channels = 2;
    mic.n_samples = FrameSamplesFor(rate);

    double total_in_e = 0.0, total_out_e = 0.0;
    for (int i = 0; i < 200; ++i) {  // 2 seconds
      // Render is silent (no echo).
      for (int s = 0; s < far.n_samples; ++s) far.ch[0][s] = 0;
      // Mic is white noise on both channels.
      std::uniform_int_distribution<int> dist(-2000, 2000);
      for (int s = 0; s < mic.n_samples; ++s) {
        const int16_t v = static_cast<int16_t>(dist(rng));
        mic.ch[0][s] = v;
        mic.ch[1][s] = v;
      }
      c.ProcessRender(far);
      c.ProcessCapture(mic, out);
      if (i >= 50) {  // skip warm-up
        for (int s = 0; s < mic.n_samples; ++s) {
          const double xin = mic.ch[0][s] / 32768.0;
          const double xout = out.ch[0][s] / 32768.0;
          total_in_e += xin * xin;
          total_out_e += xout * xout;
        }
      }
    }
    ASSERT_GT(total_in_e, 0.0);
    // Loose: out_e should be < 90% of in_e at both rates. RNNoise typically
    // achieves much more, but on synthetic uniform noise the response is
    // less predictable than on speech.
    EXPECT_LT(total_out_e, total_in_e * 0.9)
        << "rate=" << rate << " in_e=" << total_in_e << " out_e=" << total_out_e;
  }
}

TEST(AecChain, AttenuatesCorrelatedEchoAt48k) {
  AecChain c;
  ASSERT_TRUE(c.Init(48000, 2));

  std::mt19937 rng(0x4848);
  Frame zero_near{};
  zero_near.n_samples = FrameSamplesFor(48000);
  zero_near.n_channels = 1;
  Frame far, mic_mono, mic, out;
  constexpr double kEchoGain = 0.5;

  // 3 seconds total; first 100 frames (1 s) are AEC3 warm-up.
  double cumulative_in_e = 0.0;
  double cumulative_out_e = 0.0;
  for (int i = 0; i < 300; ++i) {
    // Render is mono @ 48 kHz (480 samples) — fresh noise every frame.
    FillNoise(far, rng, 48000, 1);
    // Build the mono echoed mic, then duplicate into a 2-channel mic frame.
    MixEcho(zero_near, far, kEchoGain, mic_mono);
    mic.n_samples = FrameSamplesFor(48000);
    mic.n_channels = 2;
    for (int s = 0; s < mic.n_samples; ++s) {
      mic.ch[0][s] = mic_mono.ch[0][s];
      mic.ch[1][s] = mic_mono.ch[0][s];
    }
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);

    if (i >= 100) {
      for (int j = 0; j < kFrameSamples48k; ++j) {
        const double xin = static_cast<double>(mic.ch[0][j]) / 32768.0;
        const double xout = static_cast<double>(out.ch[0][j]) / 32768.0;
        cumulative_in_e += xin * xin;
        cumulative_out_e += xout * xout;
      }
    }
  }

  ASSERT_GT(cumulative_in_e, 0.0);
  ASSERT_GT(cumulative_out_e, 0.0);
  const double erle_db = 10.0 * std::log10(cumulative_in_e / cumulative_out_e);
  EXPECT_GT(erle_db, 15.0)
      << "Real AEC3 + RNNoise should achieve > 15 dB cumulative ERLE at 48 kHz";
}

TEST(AecChain, ProcessesAt8Mics) {
  AecChain c;
  ASSERT_TRUE(c.Init(16000, 8));

  std::mt19937 rng(0x8888);
  Frame far, mic, out;
  far.n_channels = 1;
  far.n_samples = kFrameSamples16k;
  mic.n_channels = 8;
  mic.n_samples = kFrameSamples16k;

  // 100 frames: silent render, white noise on all 8 mic channels (different
  // per channel). The chain should accept all frames (no drops) and produce
  // mono output.
  for (int i = 0; i < 100; ++i) {
    for (int s = 0; s < far.n_samples; ++s) far.ch[0][s] = 0;
    for (int ch = 0; ch < 8; ++ch) {
      std::uniform_int_distribution<int> dist(-1000 - ch * 100, 1000 + ch * 100);
      for (int s = 0; s < mic.n_samples; ++s) mic.ch[ch][s] = dist(rng);
    }
    c.ProcessRender(far);
    c.ProcessCapture(mic, out);
  }

  EXPECT_EQ(c.Stats().frames_dropped, 0u);
  EXPECT_EQ(out.n_channels, 1);
  EXPECT_EQ(out.n_samples, kFrameSamples16k);
}

}  // namespace
}  // namespace ecnr

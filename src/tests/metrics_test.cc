#include "eval/metrics.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace ecnr::eval {
namespace {

constexpr int kFrameSamples = 160;  // 10 ms at 16 kHz

// Synthesise a constant-amplitude sine over `n` samples at `amplitude`.
std::vector<int16_t> SynthSine(int n, double freq_hz, double amplitude,
                               int sample_rate_hz, int phase_samples = 0) {
  std::vector<int16_t> out(n);
  constexpr double kPi = 3.14159265358979323846;
  const double w = 2.0 * kPi * freq_hz /
                   static_cast<double>(sample_rate_hz);
  for (int i = 0; i < n; ++i) {
    const double v = amplitude * std::sin(w * static_cast<double>(i + phase_samples));
    out[i] = static_cast<int16_t>(v);
  }
  return out;
}

// -----------------------------------------------------------------------------
// Rms / RmsDbfs
// -----------------------------------------------------------------------------

TEST(Metrics, RmsOfZeroBufferIsZero) {
  std::vector<int16_t> z(160, 0);
  EXPECT_DOUBLE_EQ(Rms(z.data(), 160), 0.0);
}

TEST(Metrics, RmsOfDcBufferIsAmplitude) {
  std::vector<int16_t> dc(160, 1000);
  // DC at amplitude 1000 → RMS = 1000.
  EXPECT_NEAR(Rms(dc.data(), 160), 1000.0, 1e-6);
}

TEST(Metrics, RmsOfFullScaleSineIsAboutOverSqrt2) {
  // 1 kHz sine at half full-scale, 16 kHz, one full second (≥ many cycles)
  // → RMS ≈ 16000 / sqrt(2) ≈ 11314.
  const auto sine = SynthSine(16000, 1000.0, 16000.0, 16000);
  const double rms = Rms(sine.data(), 16000);
  const double expected = 16000.0 / std::sqrt(2.0);
  EXPECT_NEAR(rms, expected, expected * 0.01);
}

TEST(Metrics, RmsDbfsOfSilenceIsNegativeInfinity) {
  std::vector<int16_t> z(160, 0);
  EXPECT_TRUE(std::isinf(RmsDbfs(z.data(), 160)));
  EXPECT_LT(RmsDbfs(z.data(), 160), 0.0);
}

TEST(Metrics, RmsDbfsOfHalfFullScaleIsAroundMinus9) {
  // RMS = 32767/2, dBFS = 20·log10(0.5) ≈ -6.02 dBFS for DC,
  // and sine at half-fullscale amplitude has RMS = amp/sqrt(2),
  // dBFS = 20·log10(amp/sqrt(2)/32767) ≈ -9.03.
  const auto sine = SynthSine(16000, 1000.0, 16383.0, 16000);
  const double dbfs = RmsDbfs(sine.data(), 16000);
  EXPECT_NEAR(dbfs, -9.03, 0.1);
}

// -----------------------------------------------------------------------------
// ErleDb
// -----------------------------------------------------------------------------

TEST(Metrics, ErleDbOfPerfectCancellationReturnsCap) {
  // Residual is exactly 0 — represents perfect cancellation. ErleDb
  // clamps to +cap_db rather than returning +inf.
  auto erle = ErleDb(/*rms_input=*/10000.0, /*rms_residual=*/0.0,
                     /*cap_db=*/80.0);
  ASSERT_TRUE(erle.has_value());
  EXPECT_DOUBLE_EQ(*erle, 80.0);
}

TEST(Metrics, ErleDbOfNoCancellationIsZero) {
  // Residual == input → ERLE = 20·log10(1) = 0 dB.
  auto erle = ErleDb(10000.0, 10000.0);
  ASSERT_TRUE(erle.has_value());
  EXPECT_NEAR(*erle, 0.0, 1e-9);
}

TEST(Metrics, ErleDbOfHalfResidualIsAboutSixDb) {
  // Residual = half input → ERLE = 20·log10(2) ≈ 6.02 dB.
  auto erle = ErleDb(10000.0, 5000.0);
  ASSERT_TRUE(erle.has_value());
  EXPECT_NEAR(*erle, 6.02, 0.01);
}

TEST(Metrics, ErleDbCanGoNegativeIfChainMakesThingsWorse) {
  // AEC3 misbehaving: residual louder than input. Returned ERLE is
  // negative (clamped to -cap_db at the floor). Tuning loop relies on
  // this so over-aggressive configs are visible.
  auto erle = ErleDb(1000.0, 100000.0);
  ASSERT_TRUE(erle.has_value());
  EXPECT_LT(*erle, 0.0);
}

TEST(Metrics, ErleDbReturnsNulloptForZeroInput) {
  auto erle = ErleDb(0.0, 100.0);
  EXPECT_FALSE(erle.has_value()) << "zero input means we have no reference";
}

// -----------------------------------------------------------------------------
// ErleAccumulator
// -----------------------------------------------------------------------------

TEST(Metrics, AccumulatorRespectsSettleWindow) {
  // 5-frame settle, push 5 frames of noisy 0-dB ERLE + 5 frames of clean
  // 6-dB ERLE. After settle, the 5 valid frames should produce a tight
  // median around 6 dB.
  ErleAccumulator a(/*settle_frames=*/5, /*input_gate_dbfs=*/-90.0);
  const auto echo  = SynthSine(kFrameSamples, 1000.0, 8000.0, 16000);
  const auto noise = SynthSine(kFrameSamples, 1000.0, 8000.0, 16000);
  const auto clean = SynthSine(kFrameSamples, 1000.0, 4000.0, 16000);  // residual = half

  for (int f = 0; f < 5; ++f) {
    a.Push(echo.data(), noise.data(), kFrameSamples);  // 0 dB ERLE, settle-skipped
  }
  for (int f = 0; f < 5; ++f) {
    a.Push(echo.data(), clean.data(), kFrameSamples);  // 6 dB ERLE
  }
  const auto s = a.Finalize();
  EXPECT_EQ(s.frames_skipped_settle, 5);
  EXPECT_EQ(s.frames_used, 5);
  ASSERT_TRUE(s.median_db.has_value());
  EXPECT_NEAR(*s.median_db, 6.02, 0.5);
}

TEST(Metrics, AccumulatorGatesLowInputFrames) {
  // Gate at -50 dBFS; push 10 frames of near-silent echo input (which
  // would otherwise produce noise-dominated ERLE numbers). Result:
  // zero valid frames, no percentiles.
  ErleAccumulator a(/*settle_frames=*/0, /*input_gate_dbfs=*/-50.0);
  const auto quiet = SynthSine(kFrameSamples, 1000.0, 50.0, 16000);
  const auto residual = SynthSine(kFrameSamples, 1000.0, 10.0, 16000);
  for (int f = 0; f < 10; ++f) {
    a.Push(quiet.data(), residual.data(), kFrameSamples);
  }
  const auto s = a.Finalize();
  EXPECT_EQ(s.frames_used, 0);
  EXPECT_EQ(s.frames_below_gate, 10);
  EXPECT_FALSE(s.median_db.has_value());
}

TEST(Metrics, AccumulatorPercentilesReflectFrameDistribution) {
  // Push 100 frames with ERLE linearly varying from 0 to 20 dB. Expect
  // median ≈ 10, p10 ≈ 2, p90 ≈ 18 (within interpolation tolerance).
  ErleAccumulator a(/*settle_frames=*/0, /*input_gate_dbfs=*/-90.0);
  const auto echo = SynthSine(kFrameSamples, 1000.0, 8000.0, 16000);
  for (int f = 0; f < 100; ++f) {
    // ERLE_target = f * 20/99 dB → residual = echo / 10^(target/20).
    const double erle_target_db = static_cast<double>(f) * 20.0 / 99.0;
    const double scale = std::pow(10.0, -erle_target_db / 20.0);
    std::vector<int16_t> residual(kFrameSamples);
    for (int i = 0; i < kFrameSamples; ++i) {
      residual[i] = static_cast<int16_t>(echo[i] * scale);
    }
    a.Push(echo.data(), residual.data(), kFrameSamples);
  }
  const auto s = a.Finalize();
  EXPECT_EQ(s.frames_used, 100);
  ASSERT_TRUE(s.median_db.has_value());
  ASSERT_TRUE(s.p10_db.has_value());
  ASSERT_TRUE(s.p90_db.has_value());
  EXPECT_NEAR(*s.median_db, 10.0, 0.5);
  EXPECT_NEAR(*s.p10_db, 2.0, 0.5);
  EXPECT_NEAR(*s.p90_db, 18.0, 0.5);
}

}  // namespace
}  // namespace ecnr::eval

#include "pipeline/beamformer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "core/frame.h"
#include "pipeline/mic_geometry.h"

namespace ecnr {
namespace {

TEST(Beamformer, InitRejectsUnsupportedSampleRate) {
  Beamformer b;
  EXPECT_FALSE(b.Init(8000, 4));
  EXPECT_FALSE(b.Init(44100, 4));
  EXPECT_FALSE(b.Init(32000, 4));
  EXPECT_TRUE(b.Init(16000, 4));
  EXPECT_TRUE(b.Init(48000, 4));
}

TEST(Beamformer, InitRejectsUnsupportedMicCount) {
  Beamformer b;
  EXPECT_FALSE(b.Init(16000, 0));
  EXPECT_FALSE(b.Init(16000, 1));   // ADR-0004: degenerate beamformer; rejected
  EXPECT_FALSE(b.Init(16000, 9));
  EXPECT_FALSE(b.Init(16000, -1));
  EXPECT_TRUE(b.Init(16000, 2));
  EXPECT_TRUE(b.Init(16000, 4));
  EXPECT_TRUE(b.Init(16000, 8));
}

TEST(Beamformer, ProcessCollapsesToCh0Verbatim_16k_4Mics) {
  // Stub Beamformer copies ch[0] verbatim; ch[1..N-1] must NOT bleed into out.
  Beamformer b;
  ASSERT_TRUE(b.Init(16000, 4));

  std::mt19937 rng(0xb44b);
  Frame in;
  in.n_samples = kFrameSamples16k;
  in.n_channels = 4;
  for (int ch = 0; ch < 4; ++ch) {
    // Distinct per-channel content — different mean to make bleed obvious.
    std::uniform_int_distribution<int> dist(-1000 - ch * 100, 1000 + ch * 100);
    for (int s = 0; s < in.n_samples; ++s) in.ch[ch][s] = dist(rng);
  }

  Frame out;
  b.Process(in, out);

  EXPECT_EQ(out.n_channels, 1);
  EXPECT_EQ(out.n_samples, kFrameSamples16k);
  for (int s = 0; s < in.n_samples; ++s) {
    EXPECT_EQ(out.ch[0][s], in.ch[0][s])
        << "stub Beamformer should pass ch[0] through; sample " << s;
  }
}

TEST(Beamformer, ProcessIndependentOfNonCh0Channels) {
  // Modifying ch[1..N-1] must not change the output. This is the contract
  // that the chain-level test couldn't recover after AEC3 landed.
  Beamformer b;
  ASSERT_TRUE(b.Init(16000, 3));

  Frame in_a;
  in_a.n_samples = kFrameSamples16k;
  in_a.n_channels = 3;
  for (int s = 0; s < in_a.n_samples; ++s) {
    in_a.ch[0][s] = static_cast<int16_t>(s);          // canonical content
    in_a.ch[1][s] = static_cast<int16_t>(1000 + s);
    in_a.ch[2][s] = static_cast<int16_t>(-1000 - s);
  }

  Frame in_b = in_a;  // copy
  // Replace ch[1] and ch[2] entirely with different content.
  for (int s = 0; s < in_b.n_samples; ++s) {
    in_b.ch[1][s] = static_cast<int16_t>(7777);
    in_b.ch[2][s] = static_cast<int16_t>(-7777);
  }

  Frame out_a, out_b;
  b.Process(in_a, out_a);
  b.Process(in_b, out_b);

  ASSERT_EQ(out_a.n_channels, 1);
  ASSERT_EQ(out_b.n_channels, 1);
  for (int s = 0; s < kFrameSamples16k; ++s) {
    EXPECT_EQ(out_a.ch[0][s], out_b.ch[0][s])
        << "ch[1..N-1] content must not affect Beamformer output; sample " << s;
  }
}

TEST(Beamformer, ProcessAt48kAlsoCollapses) {
  // Same contract at 48k. Confirms multi-rate parity.
  Beamformer b;
  ASSERT_TRUE(b.Init(48000, 4));

  Frame in;
  in.n_samples = kFrameSamples48k;
  in.n_channels = 4;
  for (int s = 0; s < in.n_samples; ++s) {
    in.ch[0][s] = static_cast<int16_t>(s);
    in.ch[1][s] = static_cast<int16_t>(-s);
    in.ch[2][s] = static_cast<int16_t>(s * 2);
    in.ch[3][s] = static_cast<int16_t>(-s * 2);
  }

  Frame out;
  b.Process(in, out);
  EXPECT_EQ(out.n_channels, 1);
  EXPECT_EQ(out.n_samples, kFrameSamples48k);
  for (int s = 0; s < in.n_samples; ++s) {
    EXPECT_EQ(out.ch[0][s], in.ch[0][s]);
  }
}

TEST(Beamformer, ProcessRejectsMisshapedInput) {
  // Per the previous Task 5.5 follow-up, Process has a release-mode early
  // return on shape mismatch (sets out.n_samples = 0). Verify this contract.
  Beamformer b;
  ASSERT_TRUE(b.Init(16000, 4));

  Frame wrong_channels;
  wrong_channels.n_samples = kFrameSamples16k;
  wrong_channels.n_channels = 3;  // configured for 4

  Frame out;
  out.n_samples = 99;  // sentinel
  b.Process(wrong_channels, out);
  EXPECT_EQ(out.n_samples, 0) << "Process should signal invalid output via n_samples=0 on shape mismatch";

  Frame wrong_samples;
  wrong_samples.n_samples = kFrameSamples48k;  // wrong rate
  wrong_samples.n_channels = 4;

  out.n_samples = 99;
  b.Process(wrong_samples, out);
  EXPECT_EQ(out.n_samples, 0);
}

// -----------------------------------------------------------------------------
// ADR-0010 DSB tests
// -----------------------------------------------------------------------------
//
// Test geometry: a linear array of N mics along +x, spaced d_m apart,
// with mic c at position ((c - (N-1)/2.0) * d_m, 0, 0). Target direction
// is +x; "90° off" (the off-axis null test) corresponds to a plane wave
// arriving from +y, which produces zero inter-mic delay across the array.
// DSB applies non-zero per-mic delays steered to +x, so identical-arrival
// signals get misaligned and partially cancel.
//
// Plane-wave synthesis: for a plane wave arriving from +source_dir, the
// signal at mic c is x_c(t) = s(t + (p_c · source_dir)/c_s) — mic c at
// positive (p_c · source_dir) sees the wavefront earlier than origin.
// We approximate this with linear interpolation across a long contiguous
// source signal so the test exercises sub-sample alignment for both the
// synthesis side (per-mic delay) and the DSB compensation side.

namespace {

constexpr float kSpeedOfSoundMps = 343.0f;

// Linear array along x with `n` mics spaced `d_m` apart, centred on origin.
// Returns a MicGeometry with positions filled in (the rest left at zero).
MicGeometry LinearArrayAlongX(int n, float d_m) {
  MicGeometry g{};
  for (int c = 0; c < n; ++c) {
    g.positions_m[c] = {
        (static_cast<float>(c) - (static_cast<float>(n) - 1.0f) * 0.5f) * d_m,
        0.0f, 0.0f};
  }
  g.target_direction = {1.0f, 0.0f, 0.0f};
  g.speed_of_sound_mps = kSpeedOfSoundMps;
  return g;
}

// Synthesise a per-mic frame for a plane wave from `source_dir` (unit
// vector). `source_buf` is a contiguous long buffer of source samples at
// `sample_rate_hz`; `frame_offset` is the index into source_buf
// corresponding to the start of the output frame as observed at the
// origin. The signal at mic c is x_c[n] = source[frame_offset + n +
// (p_c · source_dir)/c_s * fs], linearly interpolated for non-integer
// positions.
void SynthPlaneWaveFrame(const std::vector<float>& source_buf,
                         int frame_offset,
                         int n_samples,
                         int sample_rate_hz,
                         const MicGeometry& geom,
                         int num_mics,
                         const std::array<float, 3>& source_dir,
                         Frame* out) {
  out->n_channels = num_mics;
  out->n_samples = n_samples;
  for (int c = 0; c < num_mics; ++c) {
    const float proj = geom.positions_m[c][0] * source_dir[0] +
                       geom.positions_m[c][1] * source_dir[1] +
                       geom.positions_m[c][2] * source_dir[2];
    const float lead_samples =
        proj / kSpeedOfSoundMps * static_cast<float>(sample_rate_hz);
    for (int s = 0; s < n_samples; ++s) {
      const float src_pos =
          static_cast<float>(frame_offset + s) + lead_samples;
      const int i0 = static_cast<int>(std::floor(src_pos));
      const float frac = src_pos - static_cast<float>(i0);
      const int i0c = std::clamp(i0, 0, static_cast<int>(source_buf.size()) - 2);
      const float a = source_buf[i0c];
      const float b = source_buf[i0c + 1];
      float v = (1.0f - frac) * a + frac * b;
      if (v >  32767.0f) v =  32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      out->ch[c][s] = static_cast<int16_t>(v);
    }
  }
}

// RMS of an int16 vector over [0, n_samples). Returns 0 when n_samples <= 0.
double Rms(const int16_t* x, int n_samples) {
  if (n_samples <= 0) return 0.0;
  double acc = 0.0;
  for (int i = 0; i < n_samples; ++i) {
    const double v = static_cast<double>(x[i]);
    acc += v * v;
  }
  return std::sqrt(acc / static_cast<double>(n_samples));
}

}  // namespace

TEST(BeamformerDSB, PassthroughGeometryIsBitEqualToCh0) {
  // ADR-0010 action item test #4: kPassthroughGeometry → output bit-equal
  // to ch[0], identical to the Phase-0.5 stub behaviour.
  Beamformer b;
  ASSERT_TRUE(b.Init(16000, 4, kPassthroughGeometry));

  Frame in;
  in.n_channels = 4;
  in.n_samples = kFrameSamples16k;
  std::mt19937 rng(0xc0ffee);
  std::uniform_int_distribution<int> dist(-20000, 20000);
  for (int c = 0; c < 4; ++c) {
    for (int s = 0; s < in.n_samples; ++s) in.ch[c][s] = dist(rng);
  }

  Frame out;
  b.Process(in, out);
  ASSERT_EQ(out.n_channels, 1);
  ASSERT_EQ(out.n_samples, kFrameSamples16k);
  for (int s = 0; s < in.n_samples; ++s) {
    EXPECT_EQ(out.ch[0][s], in.ch[0][s]) << "sample " << s;
  }
}

TEST(BeamformerDSB, OnAxisPlaneWavePreservesSignal) {
  // ADR-0010 action item test #1: plane wave from target_direction →
  // SNR-preserving sum. With perfect alignment we expect output RMS to
  // be within a tight tolerance of the source RMS. Some fractional-delay
  // interpolation loss is expected (~1-2 % for a sine at the geometry
  // bandwidth); use a generous tolerance to keep the test sane.
  constexpr int kRate = 16000;
  constexpr int kN = 4;
  constexpr float kD = 0.04f;  // 4 cm
  constexpr int kNumFrames = 50;

  Beamformer b;
  const MicGeometry geom = LinearArrayAlongX(kN, kD);
  ASSERT_TRUE(b.Init(kRate, kN, geom));

  // Build a long source: 1 kHz sine at 0.5 fullscale. Long enough that
  // the synth's lead_samples is well within bounds for all frames.
  const int frame_samples = FrameSamplesFor(kRate);
  const int total_samples = (kNumFrames + 4) * frame_samples;
  std::vector<float> src(static_cast<size_t>(total_samples));
  for (int n = 0; n < total_samples; ++n) {
    src[n] = 16000.0f * std::sin(2.0 * M_PI * 1000.0 *
                                 static_cast<double>(n) / kRate);
  }

  const std::array<float, 3> source_dir{1.0f, 0.0f, 0.0f};  // = target

  // RMS of the source over the analysis window (same range used for output).
  std::vector<int16_t> src_int(static_cast<size_t>(kNumFrames * frame_samples));
  for (int i = 0; i < kNumFrames * frame_samples; ++i) {
    float v = src[i];
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    src_int[i] = static_cast<int16_t>(v);
  }
  // Skip the first frame in the analysis: DSB history is zero-initialised,
  // so the first frame has a brief startup transient (bounded by the max
  // per-mic delay, < 1 sample at this geometry/rate but compensated against
  // zero history → small amplitude dip on the first ~int_delay samples).
  const int skip_frames = 1;
  const double src_rms = Rms(src_int.data() + skip_frames * frame_samples,
                             (kNumFrames - skip_frames) * frame_samples);

  std::vector<int16_t> out_buf;
  out_buf.reserve(static_cast<size_t>(kNumFrames * frame_samples));
  for (int f = 0; f < kNumFrames; ++f) {
    Frame in, out;
    SynthPlaneWaveFrame(src, f * frame_samples, frame_samples, kRate, geom, kN,
                        source_dir, &in);
    b.Process(in, out);
    ASSERT_EQ(out.n_samples, frame_samples);
    out_buf.insert(out_buf.end(), out.ch[0].begin(),
                   out.ch[0].begin() + out.n_samples);
  }

  const double out_rms = Rms(out_buf.data() + skip_frames * frame_samples,
                             (kNumFrames - skip_frames) * frame_samples);
  // Expect within 5 % — covers fractional-delay interpolation loss and
  // any rounding from int16 quantisation.
  EXPECT_GT(out_rms, 0.95 * src_rms)
      << "on-axis DSB attenuated the target signal (src=" << src_rms
      << ", out=" << out_rms << ")";
  EXPECT_LT(out_rms, 1.05 * src_rms)
      << "on-axis DSB amplified the target signal beyond unity gain (src="
      << src_rms << ", out=" << out_rms << ")";
}

TEST(BeamformerDSB, OffAxisPlaneWaveIsAttenuated) {
  // ADR-0010 action item test #2: plane wave from 90° off → attenuated
  // by the DSB null pattern. With a 4-mic linear array along +x steered
  // to +x, and a source from +y, the inter-mic projection p_c · ŷ is
  // zero for all mics so they all receive the same signal. DSB applies
  // non-zero per-mic delays (steered to +x), misaligning them; at 2 kHz
  // and 4 cm spacing the inter-mic phase shift is ≈84° and the
  // 4-element sum cancels to ~22 dB attenuation in theory. We require
  // > 8 dB to leave generous margin for fractional-delay rounding.
  constexpr int kRate = 16000;
  constexpr int kN = 4;
  constexpr float kD = 0.04f;
  constexpr int kNumFrames = 50;
  constexpr double kToneHz = 2000.0;

  Beamformer b;
  const MicGeometry geom = LinearArrayAlongX(kN, kD);
  ASSERT_TRUE(b.Init(kRate, kN, geom));

  const int frame_samples = FrameSamplesFor(kRate);
  const int total_samples = (kNumFrames + 4) * frame_samples;
  std::vector<float> src(static_cast<size_t>(total_samples));
  for (int n = 0; n < total_samples; ++n) {
    src[n] = 16000.0f * std::sin(2.0 * M_PI * kToneHz *
                                 static_cast<double>(n) / kRate);
  }

  const std::array<float, 3> source_dir{0.0f, 1.0f, 0.0f};  // 90° off

  // Reference: the bare ch[0] signal RMS (since all mics receive the
  // same signal in this geometry, ch[0] RMS = input RMS).
  std::vector<int16_t> ch0_buf;
  std::vector<int16_t> out_buf;
  ch0_buf.reserve(static_cast<size_t>(kNumFrames * frame_samples));
  out_buf.reserve(static_cast<size_t>(kNumFrames * frame_samples));
  for (int f = 0; f < kNumFrames; ++f) {
    Frame in, out;
    SynthPlaneWaveFrame(src, f * frame_samples, frame_samples, kRate, geom, kN,
                        source_dir, &in);
    ch0_buf.insert(ch0_buf.end(), in.ch[0].begin(),
                   in.ch[0].begin() + frame_samples);
    b.Process(in, out);
    ASSERT_EQ(out.n_samples, frame_samples);
    out_buf.insert(out_buf.end(), out.ch[0].begin(),
                   out.ch[0].begin() + out.n_samples);
  }

  const int skip = 4 * frame_samples;  // skip startup transient
  const int n_an = kNumFrames * frame_samples - skip;
  const double ch0_rms = Rms(ch0_buf.data() + skip, n_an);
  const double out_rms = Rms(out_buf.data() + skip, n_an);
  ASSERT_GT(ch0_rms, 1.0);  // sanity

  const double attenuation_db = 20.0 * std::log10(ch0_rms / std::max(1.0, out_rms));
  EXPECT_GT(attenuation_db, 8.0)
      << "off-axis source not attenuated enough: ch0_rms=" << ch0_rms
      << ", out_rms=" << out_rms << ", atten=" << attenuation_db << " dB";
}

TEST(BeamformerDSB, TwoSourcesImprovesTargetToInterferenceRatio) {
  // ADR-0010 action item test #3: two-source anechoic mixture. Mix a
  // target signal (from +x, 1 kHz) and an interferer (from +y, 1.5 kHz).
  // After DSB the target should retain ~unity gain and the interferer
  // should be attenuated → SIR (target/interferer) improves relative to
  // raw ch[0].
  //
  // We measure the band-power ratio at 1 kHz vs 1.5 kHz on both ch[0]
  // and the DSB output via simple Goertzel-style sine/cosine
  // correlations.
  constexpr int kRate = 16000;
  constexpr int kN = 4;
  constexpr float kD = 0.04f;
  constexpr int kNumFrames = 100;
  constexpr double kTargetHz = 1000.0;
  constexpr double kIntfHz = 1500.0;

  Beamformer b;
  const MicGeometry geom = LinearArrayAlongX(kN, kD);
  ASSERT_TRUE(b.Init(kRate, kN, geom));

  const int frame_samples = FrameSamplesFor(kRate);
  const int total_samples = (kNumFrames + 4) * frame_samples;
  std::vector<float> tgt(total_samples), intf(total_samples);
  for (int n = 0; n < total_samples; ++n) {
    const double t = static_cast<double>(n) / kRate;
    tgt[n]  = 12000.0f * std::sin(2.0 * M_PI * kTargetHz * t);
    intf[n] = 12000.0f * std::sin(2.0 * M_PI * kIntfHz  * t + 0.7);
  }

  const std::array<float, 3> tgt_dir{1.0f, 0.0f, 0.0f};
  const std::array<float, 3> intf_dir{0.0f, 1.0f, 0.0f};

  auto goertzel_power = [&](const int16_t* x, int n, double f_hz) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
      const double ph = 2.0 * M_PI * f_hz * static_cast<double>(i) / kRate;
      re += static_cast<double>(x[i]) * std::cos(ph);
      im += static_cast<double>(x[i]) * std::sin(ph);
    }
    return (re * re + im * im) / static_cast<double>(n);
  };

  std::vector<int16_t> ch0_buf, out_buf;
  ch0_buf.reserve(static_cast<size_t>(kNumFrames * frame_samples));
  out_buf.reserve(static_cast<size_t>(kNumFrames * frame_samples));
  for (int f = 0; f < kNumFrames; ++f) {
    Frame in_tgt, in_intf, mixed, out;
    SynthPlaneWaveFrame(tgt,  f * frame_samples, frame_samples, kRate, geom, kN,
                        tgt_dir,  &in_tgt);
    SynthPlaneWaveFrame(intf, f * frame_samples, frame_samples, kRate, geom, kN,
                        intf_dir, &in_intf);
    mixed.n_channels = kN;
    mixed.n_samples = frame_samples;
    for (int c = 0; c < kN; ++c) {
      for (int s = 0; s < frame_samples; ++s) {
        int v = static_cast<int>(in_tgt.ch[c][s]) +
                static_cast<int>(in_intf.ch[c][s]);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        mixed.ch[c][s] = static_cast<int16_t>(v);
      }
    }
    ch0_buf.insert(ch0_buf.end(), mixed.ch[0].begin(),
                   mixed.ch[0].begin() + frame_samples);
    b.Process(mixed, out);
    out_buf.insert(out_buf.end(), out.ch[0].begin(),
                   out.ch[0].begin() + out.n_samples);
  }

  const int skip = 4 * frame_samples;
  const int n_an = kNumFrames * frame_samples - skip;
  const double ch0_tgt  = goertzel_power(ch0_buf.data() + skip, n_an, kTargetHz);
  const double ch0_intf = goertzel_power(ch0_buf.data() + skip, n_an, kIntfHz);
  const double out_tgt  = goertzel_power(out_buf.data() + skip, n_an, kTargetHz);
  const double out_intf = goertzel_power(out_buf.data() + skip, n_an, kIntfHz);
  ASSERT_GT(ch0_tgt, 0.0);
  ASSERT_GT(ch0_intf, 0.0);
  ASSERT_GT(out_intf, 0.0);

  const double ch0_sir_db = 10.0 * std::log10(ch0_tgt / ch0_intf);
  const double out_sir_db = 10.0 * std::log10(out_tgt / out_intf);
  EXPECT_GT(out_sir_db - ch0_sir_db, 6.0)
      << "two-source SIR improvement < 6 dB: ch0=" << ch0_sir_db
      << " dB, out=" << out_sir_db << " dB";

  // Target band-power should be roughly preserved (within 3 dB).
  const double tgt_atten_db = 10.0 * std::log10(ch0_tgt / std::max(1e-9, out_tgt));
  EXPECT_LT(std::abs(tgt_atten_db), 3.0)
      << "target signal not preserved within 3 dB: atten=" << tgt_atten_db << " dB";
}

}  // namespace
}  // namespace ecnr

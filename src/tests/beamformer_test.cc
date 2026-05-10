#include "pipeline/beamformer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

#include "core/frame.h"

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

}  // namespace
}  // namespace ecnr

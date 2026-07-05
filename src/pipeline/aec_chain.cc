#include "pipeline/aec_chain.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/frame.h"
#include "pipeline/aec3_adapter.h"
#include "pipeline/agc2_adapter.h"
#include "pipeline/beamformer.h"
#include "pipeline/rnnoise_adapter.h"

namespace ecnr {

namespace {
// Mode-controller constants (GB/T 45314 §5.7 echo-aware NS gate).
// Render counts as active above −50 dBFS frame RMS; the hangover covers
// the echo tail after render stops (cabin RT60 is short — ADR-0002 — and
// AEC3 models 52 ms; 300 ms is a comfortable envelope).
constexpr double kRenderActiveMeanSquare =
    (32768.0 * 0.00316) * (32768.0 * 0.00316);  // −50 dBFS
constexpr int kRenderHangoverFrames = 30;       // × 10 ms = 300 ms
// Post/pre-AEC amplitude ratio mapping to the gate. Measured on the GB/T
// conditions (2026-07-05): echo-only frames sit at r ≈ 0.013 (p50), DT
// frames with a preserved near end reach 0.2+. Between the rails the gate
// opens linearly.
constexpr float kGateRatioLo = 0.05f;
constexpr float kGateRatioHi = 0.25f;
// Asymmetric one-pole on the gate: fast attack so near-end onsets open it
// within ~2 frames, moderate decay so residual-echo mopping re-engages
// quickly after the near end stops without chattering.
constexpr float kGateAttack = 0.5f;
constexpr float kGateDecay = 0.3f;
// Leaky open-time budget: an abrupt echo-path change (GB/T 45314 §5.5.3)
// makes AEC3 leak echo for the reconvergence window, which looks exactly
// like near-end onset to the post/pre ratio — without a cap the gate stays
// open and NS mopping disengages precisely when the linear filter is at
// its weakest (measured 2026-07-05: erle_time_variation 8.5 dB > the 6 dB
// clause limit). Near-end speech is syllabic — bursts of 200-400 ms with
// pauses that replenish the budget — while reconvergence leak is one
// sustained plateau, so a ~600 ms cap separates the two. Long uninterrupted
// near-end monologue loses gate protection for its tail; §5.7 grade 2b
// tolerates partial clipping, and the −12 dB level floor holds regardless
// via the blend's `low` endpoint.
constexpr int kGateBudgetFrames = 60;        // 600 ms of continuous open
constexpr float kGateOpenThreshold = 0.5f;   // draining when above
constexpr float kGateReplenishBelow = 0.3f;  // target below this replenishes
constexpr int kGateReplenishPerFrame = 2;    // full refill in ~300 ms
}  // namespace

struct AecChain::Impl {
  Beamformer beamformer;
  Aec3Adapter aec3;
  RnNsAdapter ns;
  Agc2Adapter agc;
  bool agc_enabled = false;        // post-NS AGC stage; default OFF
  float agc_max_gain_db = 50.0f;   // WebRTC default; override via SetAgcMaxGainDb
  int aec_filter_blocks = 0;       // 0 = WebRTC default (13); see SetAecFilterLengthBlocks
  ChainStats stats;
  int sample_rate_hz = 0;
  int num_mics = 0;
  int stream_delay_ms = 0;
  // Mode-controller state: capture frames remaining in the "echo possible"
  // window, the smoothed echo gate forwarded to the NS blend, and the
  // leaky open-time budget bounding reconvergence leaks.
  int render_hangover_frames = 0;
  float echo_gate_smoothed = 1.0f;
  int gate_budget_frames = kGateBudgetFrames;
};

AecChain::AecChain() : impl_(std::make_unique<Impl>()) {}
AecChain::~AecChain() = default;

bool AecChain::Init(int sample_rate_hz, int num_mics) {
  return Init(sample_rate_hz, num_mics, kPassthroughGeometry);
}

bool AecChain::Init(int sample_rate_hz, int num_mics,
                    const MicGeometry& geometry) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;
  if (!IsSupportedMicCount(num_mics)) return false;
  if (!impl_->beamformer.Init(sample_rate_hz, num_mics, geometry)) return false;
  impl_->aec3.SetFilterLengthBlocks(impl_->aec_filter_blocks);
  if (!impl_->aec3.Init(sample_rate_hz)) return false;
  if (!impl_->ns.Init(sample_rate_hz)) return false;
  if (!impl_->agc.Init(sample_rate_hz, impl_->agc_max_gain_db)) return false;
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
  if (render.n_channels != 1 ||
      render.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    std::fprintf(stderr,
        "AecChain::ProcessRender: dropping frame (n_channels=%d expected=1, n_samples=%d expected=%d)\n",
        render.n_channels, render.n_samples,
        FrameSamplesFor(impl_->sample_rate_hz));
    ++impl_->stats.frames_dropped;
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();
  impl_->aec3.ProcessRender(render);
  const auto t1 = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(t1 - t0).count();
  impl_->stats.cpu_time_s += dt;
  impl_->stats.cpu_render_s += dt;

  // Mode controller: mark the echo-possible window while the downlink is
  // audible (plus hangover for the cabin echo tail).
  double sum_sq = 0.0;
  for (int i = 0; i < render.n_samples; ++i) {
    sum_sq += static_cast<double>(render.ch[0][i]) * render.ch[0][i];
  }
  if (sum_sq / render.n_samples > kRenderActiveMeanSquare) {
    impl_->render_hangover_frames = kRenderHangoverFrames;
  }
}

void AecChain::ProcessCapture(const Frame& mic_in, Frame& uplink_out) {
  ProcessCaptureWithTaps(mic_in, uplink_out, {});
}

void AecChain::ProcessCaptureWithTaps(const Frame& mic_in, Frame& uplink_out,
                                      const AecStageTaps& taps) {
  assert(mic_in.n_channels == impl_->num_mics);
  assert(mic_in.n_samples == FrameSamplesFor(impl_->sample_rate_hz));
  if (mic_in.n_channels != impl_->num_mics ||
      mic_in.n_samples != FrameSamplesFor(impl_->sample_rate_hz)) {
    std::fprintf(stderr,
        "AecChain::ProcessCapture: dropping frame (n_channels=%d expected=%d, n_samples=%d expected=%d)\n",
        mic_in.n_channels, impl_->num_mics, mic_in.n_samples,
        FrameSamplesFor(impl_->sample_rate_hz));
    ++impl_->stats.frames_dropped;
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();

  // Forward the latest stream-delay seed before processing so AEC3's delay
  // estimator has the freshest hint for this capture frame.
  impl_->aec3.SetStreamDelayMs(impl_->stream_delay_ms);

  Frame post_bf;
  impl_->beamformer.Process(mic_in, post_bf);
  // Stage timestamps are taken immediately after each stage call so the
  // per-stage split reflects pure stage cost; tap Frame copies (diagnostic
  // paths only — production passes {}) land in the following bucket.
  const auto t_bf = std::chrono::steady_clock::now();
  if (taps.post_beamformer) *taps.post_beamformer = post_bf;

  Frame post_aec;
  impl_->aec3.ProcessCapture(post_bf, post_aec);
  const auto t_aec = std::chrono::steady_clock::now();
  if (taps.post_aec) *taps.post_aec = post_aec;

  // Mode controller (GB/T 45314 §5.7): how much of this frame survived
  // AEC3? While render is recently active, a low post/pre ratio means the
  // frame was echo-dominated — close the NS blend gate so RNNoise mops the
  // residual at full strength. A high ratio means near-end speech is
  // present — open the gate so the VAD blend can protect it. With render
  // idle no echo is possible; the gate stays open and RNNoise's own VAD
  // is the only authority. Uniform blends are unaffected (see
  // RnNsAdapter::SetEchoGate).
  float gate_target = 1.0f;
  if (impl_->render_hangover_frames > 0) {
    --impl_->render_hangover_frames;
    double pre_sq = 0.0, post_sq = 0.0;
    for (int i = 0; i < post_bf.n_samples; ++i) {
      pre_sq += static_cast<double>(post_bf.ch[0][i]) * post_bf.ch[0][i];
      post_sq += static_cast<double>(post_aec.ch[0][i]) * post_aec.ch[0][i];
    }
    if (pre_sq > 0.0) {
      const float r = static_cast<float>(std::sqrt(post_sq / pre_sq));
      gate_target = std::clamp(
          (r - kGateRatioLo) / (kGateRatioHi - kGateRatioLo), 0.0f, 1.0f);
    }
    // pre_sq == 0: silent capture — nothing to protect or suppress; leave
    // the gate at its open target.

    // Leaky budget (constants above): sustained-open drains, quiet/echo-
    // dominant frames replenish. Exhausted budget forces the gate shut so
    // reconvergence leaks stay bounded to ~kGateBudgetFrames.
    if (gate_target < kGateReplenishBelow) {
      impl_->gate_budget_frames = std::min(
          kGateBudgetFrames, impl_->gate_budget_frames + kGateReplenishPerFrame);
    }
    if (impl_->echo_gate_smoothed > kGateOpenThreshold) {
      impl_->gate_budget_frames = std::max(0, impl_->gate_budget_frames - 1);
    }
    if (impl_->gate_budget_frames == 0) {
      gate_target = 0.0f;
    }
  } else {
    // Render idle: no echo possible, no reason to ration the gate.
    impl_->gate_budget_frames = kGateBudgetFrames;
  }
  const float k = gate_target > impl_->echo_gate_smoothed ? kGateAttack
                                                          : kGateDecay;
  impl_->echo_gate_smoothed += k * (gate_target - impl_->echo_gate_smoothed);
  impl_->ns.SetEchoGate(impl_->echo_gate_smoothed);

  impl_->ns.Process(post_aec);
  const auto t_ns = std::chrono::steady_clock::now();
  if (taps.post_ns) *taps.post_ns = post_aec;

  // Post-NS AGC (ADR-0001 architecture). Off by default to preserve
  // historical bench/live behaviour; ecnr_bench / ecnr_live opt in.
  if (impl_->agc_enabled) {
    impl_->agc.Process(post_aec);
  }
  const auto t_agc = std::chrono::steady_clock::now();
  impl_->stats.cpu_bf_s  += std::chrono::duration<double>(t_bf - t0).count();
  impl_->stats.cpu_aec_s += std::chrono::duration<double>(t_aec - t_bf).count();
  impl_->stats.cpu_ns_s  += std::chrono::duration<double>(t_ns - t_aec).count();
  impl_->stats.cpu_agc_s += std::chrono::duration<double>(t_agc - t_ns).count();
  if (taps.post_agc) *taps.post_agc = post_aec;

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

  // Pull APM-internal stats and copy field-for-field into ChainStats. Per
  // ADR-0006, ChainStats's optional fields mirror webrtc::AudioProcessingStats.
  // APM populates these gradually (delay metrics aggregate over ~1 s windows).
  const Aec3Adapter::Stats a = impl_->aec3.GetStats();
  impl_->stats.echo_return_loss_enhancement_db     = a.echo_return_loss_enhancement_db;
  impl_->stats.echo_return_loss_db                 = a.echo_return_loss_db;
  impl_->stats.residual_echo_likelihood            = a.residual_echo_likelihood;
  impl_->stats.residual_echo_likelihood_recent_max = a.residual_echo_likelihood_recent_max;
  impl_->stats.delay_ms                            = a.delay_ms;
  impl_->stats.delay_median_ms                     = a.delay_median_ms;
  impl_->stats.divergent_filter_fraction           = a.divergent_filter_fraction;
  // NS diagnostics. Always populated after at least one ProcessCapture; the
  // values are valid even when the chain is in pre-mitigation behaviour
  // (vad_prob still reflects RNNoise's internal VAD, just unused for the
  // blend computation when low == high == 0).
  impl_->stats.ns_vad_prob       = impl_->ns.LastVadProb();
  impl_->stats.ns_current_blend  = impl_->ns.CurrentBlend();
}

void AecChain::Reset() {
  impl_->beamformer.Reset();
  impl_->aec3.Reset();
  impl_->ns.Reset();
  impl_->agc.Reset();
  impl_->stats = {};
  impl_->render_hangover_frames = 0;
  impl_->echo_gate_smoothed = 1.0f;
}

void AecChain::SetStreamDelayMs(int delay_ms) {
  impl_->stream_delay_ms = std::clamp(delay_ms, 0, kMaxStreamDelayMs);
}

void AecChain::SetNsDryBlend(float blend) {
  impl_->ns.SetDryBlend(blend);
}

void AecChain::SetNsVadBlendRange(float low, float high) {
  impl_->ns.SetVadBlendRange(low, high);
}

void AecChain::SetAgcEnabled(bool enabled) {
  impl_->agc_enabled = enabled;
}

void AecChain::SetAecFilterLengthBlocks(int blocks) {
  // Stored for the next Init(), same lifecycle as SetAgcMaxGainDb.
  impl_->aec_filter_blocks = blocks;
}

void AecChain::SetAecDtTuning(const AecDtTuning& tuning) {
  // Forwarded immediately; the adapter stores it until its Init() bakes
  // the config into the EchoControlFactory (same lifecycle as above).
  impl_->aec3.SetDtTuning(tuning);
}

void AecChain::SetAgcMaxGainDb(float max_gain_db) {
  // Stored for the next Init(). If called after Init(), takes effect only
  // on a future Init() — the running APM's config is fixed at construction.
  impl_->agc_max_gain_db = max_gain_db;
}

const ChainStats& AecChain::Stats() const { return impl_->stats; }

}  // namespace ecnr

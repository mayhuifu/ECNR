#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/frame.h"

namespace ecnr {

// Conservative upper bound for the render→capture delay seed we forward to
// AEC3. Mirrors WebRTC AudioProcessingImpl::set_stream_delay_ms which clamps
// to the same value (audio_processing_impl.cc, see TODO in upstream noting
// the bound is somewhat arbitrary). Values above are clamped down.
constexpr int kMaxStreamDelayMs = 500;

// Aggregated runtime stats for the chain. Updated each ProcessCapture call.
// Optional fields mirror webrtc::AudioProcessingStats field-for-field
// (per ADR-0006); under the Phase-0 stub they all stay nullopt and are
// populated by the WebRTC APM backend wired in Task 6.
struct ChainStats {
  // Cumulative wallclock seconds spent in ProcessRender + ProcessCapture.
  double cpu_time_s = 0.0;
  // Cumulative seconds of audio passed through the chain.
  double audio_time_s = 0.0;

  // ERLE = 10*log10(P_echo / P_out). Mirrors
  // webrtc::AudioProcessingStats::echo_return_loss_enhancement.
  std::optional<double> echo_return_loss_enhancement_db;
  // ERL = 10*log10(P_far / P_echo). Mirrors
  // webrtc::AudioProcessingStats::echo_return_loss.
  std::optional<double> echo_return_loss_db;
  // Likelihood [0..1] that residual echo is present. Mirrors
  // residual_echo_likelihood.
  std::optional<double> residual_echo_likelihood;
  // Recent maximum of residual_echo_likelihood. Mirrors
  // residual_echo_likelihood_recent_max.
  std::optional<double> residual_echo_likelihood_recent_max;
  // AEC3's current delay estimate in ms. Mirrors delay_ms.
  std::optional<int32_t> delay_ms;
  // Median observed delay. Mirrors delay_median_ms.
  std::optional<int32_t> delay_median_ms;
  // Fraction of time the adaptive filter is divergent. Mirrors
  // divergent_filter_fraction.
  std::optional<double> divergent_filter_fraction;

  // Counts capture/render frames the chain refused due to shape mismatches
  // (wrong rate, wrong channel count, wrong frame size). Should always be 0
  // in healthy callers; non-zero is a bug in upstream HAL or harness code.
  uint64_t frames_dropped = 0;

  // RNNoise's smoothed voice-activity probability for the most recent
  // processed frame, in [0, 1]. nullopt until ProcessCapture has been
  // called at least once. See RnNsAdapter::LastVadProb / SetVadBlendRange
  // (Step B of the NS over-suppression mitigation tracked in PROJECT.md).
  std::optional<float> ns_vad_prob;
  // Per-frame blend α actually applied in the wet/dry mix (interpolated
  // between vad_blend_low and vad_blend_high by ns_vad_prob). Useful for
  // bench logging and confirming the blend is being modulated as expected.
  std::optional<float> ns_current_blend;

  // Real-Time Factor: cpu_time / audio_time. < 1.0 = faster than realtime.
  double Rtf() const {
    return audio_time_s > 0.0 ? cpu_time_s / audio_time_s : 0.0;
  }
};

// AEC + NS chain. Phase 0 ships stub backends behind this interface; Phase 0.5
// swaps in WebRTC AEC3 + RNNoise without changing callers.
class AecChain {
 public:
  AecChain();
  ~AecChain();

  AecChain(const AecChain&) = delete;
  AecChain& operator=(const AecChain&) = delete;

  // Sample rate must be 16000 or 48000 (see IsSupportedSampleRate); num_mics
  // must be in [2, kMaxMics] (see IsSupportedMicCount). The chain ingests
  // per-mic capture frames and emits mono uplink. Per ADR-0003 + ADR-0004.
  bool Init(int sample_rate_hz, int num_mics);

  // Push a far-end (render / loudspeaker reference) frame. Render is mono —
  // n_channels must equal 1, and n_samples must match the configured rate.
  // Must precede the matching capture frame in time. The chain's delay
  // estimator handles arbitrary mic-vs-render latency at the system boundary.
  void ProcessRender(const Frame& render);

  // Push a near-end (mic) frame; receive the cleaned mono frame in `out`.
  // mic_in.n_channels must equal the configured num_mics; uplink_out emerges
  // mono (n_channels = 1) after Beamformer collapses the multi-mic input.
  void ProcessCapture(const Frame& mic_in, Frame& uplink_out);

  // Drop adapted state — call on stream restart, sample-rate change, or
  // confirmed routing change at the HAL.
  void Reset();

  // Inform the chain of the latency between the render-tap signal previously
  // pushed via ProcessRender and the corresponding mic frame about to be
  // pushed via ProcessCapture. WebRTC AEC3 (wired in Task 6) uses this to
  // seed its delay estimator; passing the wrong value makes AEC fail-quiet
  // (no apparent echo cancellation). Values outside [0, kMaxStreamDelayMs]
  // are clamped (matches webrtc::AudioProcessing::set_stream_delay_ms).
  void SetStreamDelayMs(int delay_ms);

  // Cap NS suppression by mixing α·NS-input back into (1−α)·NS-output.
  // Forwards to RnNsAdapter::SetDryBlend. blend ∈ [0, 1]; 0 = unchanged
  // RNNoise (default), 1 = NS bypass. blend = 0.25 caps suppression at
  // ~-12 dB, mitigating the chopped-voice / over-suppression artifact on
  // non-stationary noise scenes documented in PROJECT.md "Known
  // limitations". Real-time safe; can be called between frames.
  void SetNsDryBlend(float blend);

  // VAD-gated NS blend (Step B of the over-suppression mitigation).
  // Selects per-frame α between `low` (noise-dominant frames) and `high`
  // (voice-dominant frames) using RNNoise's per-frame voice-activity
  // probability as the interpolation parameter, with asymmetric smoothing
  // (fast attack to voice mode, slow decay back to noise mode). Setting
  // low = high reduces to the uniform Step-A blend. Suggested production
  // pair: low = 0.0 (full RNNoise on pure-noise frames) + high = 0.30
  // (cap at ~-10 dB suppression on voice-dominant frames to avoid
  // chopping). Forwards to RnNsAdapter::SetVadBlendRange.
  void SetNsVadBlendRange(float low, float high);

  const ChainStats& Stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

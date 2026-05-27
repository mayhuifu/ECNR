// ecnr_eval — AEC3 tuning + ERLE measurement harness.
//
// Implements the ADR-0011 contract: emits two ERLE numbers per condition,
//   - erle_reported_*: AEC3's self-report (ChainStats::echo_return_loss_
//     enhancement_db, aggregated over the run after a settle window).
//   - erle_true_*:     externally computed by feeding the echo-only mic
//     track through a fresh chain instance, RMS-aggregating the residual
//     output against the echo-only input.
//
// First-cut scope (relative to ADR-0011 §4 CSV schema):
//   - CSV columns: condition_id, config_name, sample_rate_hz, then the
//     reported / true ERLE p10/median/p90 + frames_dropped + frames_used.
//   - config_hash + condition_hash columns are deferred until the TOML
//     sweep parser lands (ADR-0011 §2). Today there is one config:
//     "default-webrtc". The schema is a strict subset of the locked
//     ADR-0011 contract, never a superset.
//   - --run mode iterates condition subdirectories that contain mic.wav
//     + echo_only_mic.wav + ref.wav. Other layout members
//     (near_end_clean.wav, meta.toml) are ignored for now.
//
// Modes:
//   ecnr_eval --self-test
//     In-memory synthetic fixture; asserts erle_true_median_db is
//     comfortably positive. Used as a CI smoke and as the harness's
//     hello-world. No file I/O.
//
//   ecnr_eval --run --conditions DIR --out FILE.csv
//     Iterates DIR/*/ subdirectories, runs the chain twice per condition,
//     emits a CSV per ADR-0011 §4. Exits non-zero if any condition is
//     malformed (missing track, sample-rate mismatch, length mismatch).
//     If a condition also contains near_end_clean.wav, the CSV includes
//     double-talk near-end preservation metrics used by the GB/T 45314
//     emergency-call pre-compliance gate.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"
#include "eval/metrics.h"
#include "hal/file_io.h"
#include "pipeline/aec_chain.h"

namespace {

constexpr int kSelfTestSampleRate = 16000;
constexpr int kSelfTestSeconds    = 5;
constexpr int kSettleFrames       = 100;  // 1 s at 10 ms/frame; ADR-0011 §4 default
constexpr double kInputGateDbfs   = -50.0;

struct Args {
  bool self_test = false;
  bool run = false;
  std::string conditions_dir;
  std::string out_csv;
  std::string out_wavs_dir;  // optional; if set, write per-condition output WAVs here
  std::string stage_wavs_dir; // optional; if set, write per-stage diagnostic WAVs
  bool agc_enabled = false;  // optional post-NS AGC2 (mirrors ecnr_bench --agc)
  // RNNoise blend controls mirror ecnr_bench/ecnr_live so pre-compliance
  // gates can sweep production tuning without a separate tool path.
  float ns_dry_blend = 0.0f;
  bool ns_vad_blend_set = false;
  float ns_vad_blend_low = 0.0f;
  float ns_vad_blend_high = 0.0f;
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
      "usage: %s --self-test\n"
      "       %s --run --conditions DIR --out FILE.csv [--out-wavs WAV_DIR]\n"
      "          [--agc] [--ns-dry-blend <0..1>] | [--ns-vad-blend <low,high>]\n"
      "\n"
      "  --self-test           in-memory synthetic fixture; asserts ERLE looks healthy.\n"
      "                        No file I/O. Used as CI smoke + harness hello-world.\n"
      "  --run                 iterate condition subdirs and emit a CSV per ADR-0011 §4.\n"
      "  --conditions DIR      root of the condition tree. Each direct subdir must contain\n"
      "                        mic.wav, ref.wav, echo_only_mic.wav (all same rate + length).\n"
      "  --out FILE.csv        results path. Overwritten without prompt.\n"
      "  --out-wavs WAV_DIR    optional. When set, writes per-condition chain output\n"
      "                        as WAV_DIR/<condition_id>.wav (mic-pass A residual).\n"
      "                        Required for downstream DNSMOS / AECMOS scoring via\n"
      "                        reference/score_mos.py.\n"
      "  --stage-wavs WAV_DIR  optional. When set, writes stage-tap diagnostics as\n"
      "                        WAV_DIR/<condition_id>/{post_bf,post_aec,post_ns,post_agc}.wav.\n",
      prog, prog);
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--self-test") a->self_test = true;
    else if (flag == "--run") a->run = true;
    else if (flag == "--conditions" && i + 1 < argc) a->conditions_dir = argv[++i];
    else if (flag == "--out" && i + 1 < argc) a->out_csv = argv[++i];
    else if (flag == "--out-wavs" && i + 1 < argc) a->out_wavs_dir = argv[++i];
    else if (flag == "--stage-wavs" && i + 1 < argc) a->stage_wavs_dir = argv[++i];
    else if (flag == "--agc") a->agc_enabled = true;
    else if (flag == "--ns-dry-blend" && i + 1 < argc) {
      try {
        a->ns_dry_blend = std::stof(argv[++i]);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "could not parse --ns-dry-blend value: %s\n",
                     e.what());
        return false;
      }
    }
    else if (flag == "--ns-vad-blend" && i + 1 < argc) {
      const std::string val = argv[++i];
      const auto comma = val.find(',');
      if (comma == std::string::npos) {
        std::fprintf(stderr,
            "--ns-vad-blend expects \"low,high\" (e.g. 0.0,0.30); got: %s\n",
            val.c_str());
        return false;
      }
      try {
        a->ns_vad_blend_low = std::stof(val.substr(0, comma));
        a->ns_vad_blend_high = std::stof(val.substr(comma + 1));
        a->ns_vad_blend_set = true;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "could not parse --ns-vad-blend value '%s': %s\n",
                     val.c_str(), e.what());
        return false;
      }
    }
    else if (flag == "-h" || flag == "--help") return false;
    else { std::fprintf(stderr, "unknown arg: %s\n", flag.c_str()); return false; }
  }
  if (a->self_test == a->run) {
    // Exactly one mode is required.
    std::fprintf(stderr, "exactly one of --self-test / --run is required\n");
    return false;
  }
  if (a->run && (a->conditions_dir.empty() || a->out_csv.empty())) {
    std::fprintf(stderr, "--run requires both --conditions and --out\n");
    return false;
  }
  return true;
}

// Synthesise a 1 kHz sine into an int16 buffer.
void SynthSine(std::vector<int16_t>* out, int n_samples, double freq_hz,
               double amplitude, int sample_rate_hz) {
  out->resize(static_cast<size_t>(n_samples));
  constexpr double kPi = 3.14159265358979323846;
  const double w = 2.0 * kPi * freq_hz /
                   static_cast<double>(sample_rate_hz);
  for (int i = 0; i < n_samples; ++i) {
    const double v = amplitude * std::sin(w * static_cast<double>(i));
    (*out)[i] = static_cast<int16_t>(v);
  }
}

// Run the chain on (ref, capture) input streams and collect a series of
// per-frame post-AEC residual frames, along with per-frame AEC3-reported
// ERLE values from ChainStats. Caller pre-sizes ref + capture to the
// same length and a multiple of the frame samples.
//
// `out_residual` ends up the same length as capture (one mono sample
// per input frame sample). `out_reported_erle_db` is one entry per
// frame processed.
struct ChainRunResult {
  std::vector<int16_t> residual;
  std::vector<int16_t> post_beamformer;
  std::vector<int16_t> post_aec;
  std::vector<int16_t> post_ns;
  std::vector<int16_t> post_agc;
  std::vector<double> reported_erle_db_per_frame;  // unfilled frames omitted
  std::vector<double> residual_echo_likelihood_per_frame;  // unfilled frames omitted
  double rtf = 0.0;
};

struct ChainConfig {
  bool agc_enabled = false;
  float ns_dry_blend = 0.0f;
  bool ns_vad_blend_set = false;
  float ns_vad_blend_low = 0.0f;
  float ns_vad_blend_high = 0.0f;
};

std::string ConfigName(const ChainConfig& config) {
  std::string name = "default-webrtc";
  if (config.agc_enabled) name += "+agc";
  char blend_buf[64];
  if (config.ns_vad_blend_set) {
    std::snprintf(blend_buf, sizeof(blend_buf), "+ns_vad_%.2f_%.2f",
                  config.ns_vad_blend_low, config.ns_vad_blend_high);
  } else if (config.ns_dry_blend != 0.0f) {
    std::snprintf(blend_buf, sizeof(blend_buf), "+ns_dry_%.2f",
                  config.ns_dry_blend);
  } else {
    std::snprintf(blend_buf, sizeof(blend_buf), "%s", "");
  }
  name += blend_buf;
  return name;
}

ChainRunResult RunChain(const std::vector<int16_t>& ref,
                        const std::vector<int16_t>& capture,
                        int sample_rate_hz, int num_mics,
                        const ChainConfig& config = {},
                        bool collect_stage_taps = false) {
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);
  ecnr::AecChain chain;
  // Passthrough geometry — we don't care about beamforming for ERLE.
  // The mic path is mono-duplicated into ch[0..num_mics-1]; passthrough
  // selects ch[0].
  if (!chain.Init(sample_rate_hz, num_mics)) {
    std::fprintf(stderr, "RunChain: chain init failed (rate=%d, num_mics=%d)\n",
                 sample_rate_hz, num_mics);
    return {};
  }
  if (config.ns_vad_blend_set) {
    chain.SetNsVadBlendRange(config.ns_vad_blend_low,
                             config.ns_vad_blend_high);
  } else {
    chain.SetNsDryBlend(config.ns_dry_blend);
  }
  chain.SetAgcEnabled(config.agc_enabled);

  const size_t n_frames =
      std::min(ref.size(), capture.size()) /
      static_cast<size_t>(frame_samples);

  ChainRunResult r;
  r.residual.reserve(n_frames * static_cast<size_t>(frame_samples));
  if (collect_stage_taps) {
    const size_t samples = n_frames * static_cast<size_t>(frame_samples);
    r.post_beamformer.reserve(samples);
    r.post_aec.reserve(samples);
    r.post_ns.reserve(samples);
    r.post_agc.reserve(samples);
  }
  r.reported_erle_db_per_frame.reserve(n_frames);
  r.residual_echo_likelihood_per_frame.reserve(n_frames);

  ecnr::Frame ref_f, mic_f, out_f, post_bf_f, post_aec_f, post_ns_f, post_agc_f;
  for (size_t i = 0; i < n_frames; ++i) {
    const size_t off = i * static_cast<size_t>(frame_samples);

    ref_f.n_samples = frame_samples;
    ref_f.n_channels = 1;
    std::memcpy(ref_f.ch[0].data(), ref.data() + off,
                static_cast<size_t>(frame_samples) * sizeof(int16_t));

    mic_f.n_samples = frame_samples;
    mic_f.n_channels = num_mics;
    // Mono-duplicate the capture stream into all configured mic channels.
    // The passthrough beamformer reads ch[0]; the duplicates are harmless.
    for (int c = 0; c < num_mics; ++c) {
      std::memcpy(mic_f.ch[c].data(), capture.data() + off,
                  static_cast<size_t>(frame_samples) * sizeof(int16_t));
    }

    chain.ProcessRender(ref_f);
    if (collect_stage_taps) {
      ecnr::AecStageTaps taps;
      taps.post_beamformer = &post_bf_f;
      taps.post_aec = &post_aec_f;
      taps.post_ns = &post_ns_f;
      taps.post_agc = &post_agc_f;
      chain.ProcessCaptureWithTaps(mic_f, out_f, taps);
    } else {
      chain.ProcessCapture(mic_f, out_f);
    }

    r.residual.insert(r.residual.end(), out_f.ch[0].begin(),
                      out_f.ch[0].begin() + out_f.n_samples);
    if (collect_stage_taps) {
      r.post_beamformer.insert(r.post_beamformer.end(), post_bf_f.ch[0].begin(),
                               post_bf_f.ch[0].begin() + post_bf_f.n_samples);
      r.post_aec.insert(r.post_aec.end(), post_aec_f.ch[0].begin(),
                        post_aec_f.ch[0].begin() + post_aec_f.n_samples);
      r.post_ns.insert(r.post_ns.end(), post_ns_f.ch[0].begin(),
                       post_ns_f.ch[0].begin() + post_ns_f.n_samples);
      r.post_agc.insert(r.post_agc.end(), post_agc_f.ch[0].begin(),
                        post_agc_f.ch[0].begin() + post_agc_f.n_samples);
    }

    const auto& s = chain.Stats();
    if (s.echo_return_loss_enhancement_db.has_value()) {
      r.reported_erle_db_per_frame.push_back(*s.echo_return_loss_enhancement_db);
    }
    if (s.residual_echo_likelihood.has_value()) {
      r.residual_echo_likelihood_per_frame.push_back(*s.residual_echo_likelihood);
    }
  }
  r.rtf = chain.Stats().Rtf();
  return r;
}

// Aggregate a per-frame ERLE series into median / p10 / p90, after
// applying the settle window. Mirrors ErleAccumulator's contract for
// the AEC3-reported series — but operates on a pre-computed series of
// scalars, not on (echo, residual) pairs.
struct PercentileTriple {
  std::optional<double> median_db, p10_db, p90_db;
  int frames_used = 0;
  int frames_skipped_settle = 0;
};

PercentileTriple AggregatePercentiles(const std::vector<double>& series,
                                      int settle_frames) {
  PercentileTriple p;
  p.frames_skipped_settle = std::min(static_cast<int>(series.size()), settle_frames);
  if (static_cast<int>(series.size()) <= settle_frames) return p;
  std::vector<double> sorted(series.begin() + settle_frames, series.end());
  std::sort(sorted.begin(), sorted.end());
  p.frames_used = static_cast<int>(sorted.size());
  if (sorted.empty()) return p;
  auto pct = [&](double q) {
    if (sorted.size() == 1) return sorted[0];
    const double idx = q * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<size_t>(std::floor(idx));
    const auto hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
  };
  p.median_db = pct(0.50);
  p.p10_db    = pct(0.10);
  p.p90_db    = pct(0.90);
  return p;
}

// Compute true ERLE accumulator from a per-frame walk over the echo-only
// run: input is the synthesised echo_only_mic, residual is the post-chain
// output of feeding echo_only_mic through the chain.
ecnr::eval::ErleStats ComputeTrueErle(const std::vector<int16_t>& echo_only_mic,
                                       const std::vector<int16_t>& residual,
                                       int sample_rate_hz) {
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);
  ecnr::eval::ErleAccumulator acc(kSettleFrames, kInputGateDbfs);
  const size_t n_frames =
      std::min(echo_only_mic.size(), residual.size()) /
      static_cast<size_t>(frame_samples);
  for (size_t i = 0; i < n_frames; ++i) {
    const size_t off = i * static_cast<size_t>(frame_samples);
    acc.Push(echo_only_mic.data() + off, residual.data() + off, frame_samples);
  }
  return acc.Finalize();
}

std::vector<double> ComputePerFrameErle(const std::vector<int16_t>& echo_in,
                                        const std::vector<int16_t>& residual,
                                        int sample_rate_hz) {
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);
  const size_t n_frames =
      std::min(echo_in.size(), residual.size()) /
      static_cast<size_t>(frame_samples);
  std::vector<double> out;
  out.reserve(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    const size_t off = i * static_cast<size_t>(frame_samples);
    const double rms_in = ecnr::eval::Rms(echo_in.data() + off, frame_samples);
    const double rms_res = ecnr::eval::Rms(residual.data() + off, frame_samples);
    const auto erle = ecnr::eval::ErleDb(rms_in, rms_res);
    out.push_back(erle.value_or(0.0));
  }
  return out;
}

std::optional<double> ErleAtMs(const std::vector<double>& per_frame_erle_db,
                               int time_ms) {
  if (per_frame_erle_db.empty()) return std::nullopt;
  const size_t idx = std::min(
      static_cast<size_t>(std::max(0, time_ms) / ecnr::kFrameDurationMs),
      per_frame_erle_db.size() - 1);
  return per_frame_erle_db[idx];
}

std::optional<double> MedianErleInRange(const std::vector<double>& per_frame_erle_db,
                                        int start_ms, int end_ms) {
  if (per_frame_erle_db.empty() || end_ms <= start_ms) return std::nullopt;
  const size_t begin = std::min(
      static_cast<size_t>(std::max(0, start_ms) / ecnr::kFrameDurationMs),
      per_frame_erle_db.size());
  const size_t end = std::min(
      static_cast<size_t>(std::max(0, end_ms) / ecnr::kFrameDurationMs),
      per_frame_erle_db.size());
  if (begin >= end) return std::nullopt;
  std::vector<double> sorted(per_frame_erle_db.begin() + begin,
                             per_frame_erle_db.begin() + end);
  std::sort(sorted.begin(), sorted.end());
  return sorted[sorted.size() / 2];
}

std::optional<double> ErleWorstOneSecondMedianAfterSettle(
    const std::vector<double>& per_frame_erle_db) {
  constexpr int kWindowFrames = 100;  // 1 s at 10 ms/frame.
  if (static_cast<int>(per_frame_erle_db.size()) <= kSettleFrames + kWindowFrames) {
    return std::nullopt;
  }
  std::optional<double> worst;
  for (int start = kSettleFrames;
       start + kWindowFrames <= static_cast<int>(per_frame_erle_db.size());
       start += 10) {
    std::vector<double> window(per_frame_erle_db.begin() + start,
                               per_frame_erle_db.begin() + start + kWindowFrames);
    std::sort(window.begin(), window.end());
    const double median = window[window.size() / 2];
    if (!worst.has_value() || median < *worst) worst = median;
  }
  return worst;
}

std::optional<double> LevelRangeDb(const std::vector<int16_t>& samples,
                                   int sample_rate_hz,
                                   int window_ms,
                                   int skip_ms) {
  const int window_samples = sample_rate_hz * window_ms / 1000;
  const int skip_samples = sample_rate_hz * skip_ms / 1000;
  if (window_samples <= 0 ||
      static_cast<int>(samples.size()) < skip_samples + window_samples) {
    return std::nullopt;
  }
  std::vector<double> levels;
  for (size_t off = static_cast<size_t>(skip_samples);
       off + static_cast<size_t>(window_samples) <= samples.size();
       off += static_cast<size_t>(window_samples)) {
    const double dbfs = ecnr::eval::RmsDbfs(samples.data() + off, window_samples);
    if (std::isfinite(dbfs)) levels.push_back(dbfs);
  }
  if (levels.empty()) return std::nullopt;
  const auto [lo, hi] = std::minmax_element(levels.begin(), levels.end());
  return *hi - *lo;
}

struct NearEndStats {
  std::optional<double> level_delta_median_db;
  std::optional<double> correlation;
  int frames_used = 0;
};

NearEndStats ComputeNearEndStats(const std::vector<int16_t>& near_end_clean,
                                 const std::vector<int16_t>& residual,
                                 int sample_rate_hz) {
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);
  const size_t n_frames =
      std::min(near_end_clean.size(), residual.size()) /
      static_cast<size_t>(frame_samples);
  std::vector<double> deltas;
  double sum_x2 = 0.0;
  double sum_y2 = 0.0;
  double sum_xy = 0.0;
  NearEndStats stats;
  for (size_t f = 0; f < n_frames; ++f) {
    const size_t off = f * static_cast<size_t>(frame_samples);
    const double rms_clean =
        ecnr::eval::Rms(near_end_clean.data() + off, frame_samples);
    if (rms_clean <= 0.0 ||
        ecnr::eval::RmsDbfs(near_end_clean.data() + off, frame_samples) < -50.0) {
      continue;
    }
    const double rms_out = ecnr::eval::Rms(residual.data() + off, frame_samples);
    if (rms_out > 0.0) {
      deltas.push_back(20.0 * std::log10(rms_out / rms_clean));
    }
    for (int i = 0; i < frame_samples; ++i) {
      const double x = static_cast<double>(near_end_clean[off + i]);
      const double y = static_cast<double>(residual[off + i]);
      sum_x2 += x * x;
      sum_y2 += y * y;
      sum_xy += x * y;
    }
    ++stats.frames_used;
  }
  if (!deltas.empty()) {
    std::sort(deltas.begin(), deltas.end());
    stats.level_delta_median_db = deltas[deltas.size() / 2];
  }
  if (sum_x2 > 0.0 && sum_y2 > 0.0) {
    stats.correlation = sum_xy / std::sqrt(sum_x2 * sum_y2);
  }
  return stats;
}

int RunSelfTest() {
  // Synthetic fixture: 5 s of 1 kHz tone at half-FS as the far-end render,
  // a linearly-scaled (factor 0.5) copy as the echo arriving at the mic.
  // No near-end, no noise. AEC3 should easily cancel a static linear-
  // gain echo; we assert true-ERLE > 12 dB to give a wide bound that
  // catches chain regressions without being noise-sensitive.
  constexpr int kRate = kSelfTestSampleRate;
  const int n = kRate * kSelfTestSeconds;

  std::vector<int16_t> ref, echo_only_mic;
  SynthSine(&ref,           n, 1000.0, 16000.0, kRate);
  SynthSine(&echo_only_mic, n, 1000.0,  8000.0, kRate);  // echo = ref × 0.5

  auto run = RunChain(ref, echo_only_mic, kRate, /*num_mics=*/2);
  const auto reported = AggregatePercentiles(run.reported_erle_db_per_frame,
                                              kSettleFrames);
  const auto true_stats = ComputeTrueErle(echo_only_mic, run.residual, kRate);

  std::printf("ecnr_eval self-test (5 s, 1 kHz tone, gain-0.5 echo, 16 kHz):\n");
  std::printf("  frames processed       = %zu\n", run.residual.size() / 160);
  std::printf("  reported ERLE median   = %s dB (p10=%s, p90=%s)\n",
              reported.median_db ? std::to_string(*reported.median_db).c_str() : "N/A",
              reported.p10_db    ? std::to_string(*reported.p10_db).c_str()    : "N/A",
              reported.p90_db    ? std::to_string(*reported.p90_db).c_str()    : "N/A");
  std::printf("  true ERLE median       = %s dB (p10=%s, p90=%s)\n",
              true_stats.median_db ? std::to_string(*true_stats.median_db).c_str() : "N/A",
              true_stats.p10_db    ? std::to_string(*true_stats.p10_db).c_str()    : "N/A",
              true_stats.p90_db    ? std::to_string(*true_stats.p90_db).c_str()    : "N/A");
  std::printf("  frames used (true)     = %d\n", true_stats.frames_used);
  std::printf("  frames skipped settle  = %d\n", true_stats.frames_skipped_settle);
  std::printf("  frames below gate      = %d\n", true_stats.frames_below_gate);

  // Loose bound (ADR-0011 §6 "wide enough to catch regressions, not tuning").
  // 1 kHz tone with constant linear gain is the easiest case for AEC3;
  // <12 dB true ERLE here would indicate something is broken upstream.
  if (!true_stats.median_db.has_value() || *true_stats.median_db < 12.0) {
    std::fprintf(stderr,
        "FAIL: true ERLE median is %s (expected > 12 dB)\n",
        true_stats.median_db ? std::to_string(*true_stats.median_db).c_str()
                              : "absent");
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}

// Read a single condition. Returns true on success, false on any structural
// problem (missing file, sample-rate mismatch, length mismatch).
struct Condition {
  std::string id;
  int sample_rate_hz = 0;
  std::vector<int16_t> mic;
  std::vector<int16_t> ref;
  std::vector<int16_t> echo_only_mic;
  std::vector<int16_t> near_end_clean;
  bool has_near_end_clean = false;
};

bool LoadCondition(const std::filesystem::path& dir, Condition* c,
                   std::string* err) {
  c->id = dir.filename().string();
  ecnr::hal::WavData mic_w, ref_w, echo_w;
  if (!ecnr::hal::ReadWavMono((dir / "mic.wav").string(), &mic_w, err)) {
    *err = "read mic.wav: " + *err;
    return false;
  }
  if (!ecnr::hal::ReadWavMono((dir / "ref.wav").string(), &ref_w, err)) {
    *err = "read ref.wav: " + *err;
    return false;
  }
  if (!ecnr::hal::ReadWavMono((dir / "echo_only_mic.wav").string(), &echo_w, err)) {
    *err = "read echo_only_mic.wav: " + *err;
    return false;
  }
  if (mic_w.sample_rate_hz != ref_w.sample_rate_hz ||
      ref_w.sample_rate_hz != echo_w.sample_rate_hz) {
    *err = "sample-rate mismatch across condition tracks";
    return false;
  }
  if (!ecnr::IsSupportedSampleRate(mic_w.sample_rate_hz)) {
    *err = "unsupported sample rate: " + std::to_string(mic_w.sample_rate_hz);
    return false;
  }
  const size_t common_len = std::min({mic_w.samples.size(),
                                       ref_w.samples.size(),
                                       echo_w.samples.size()});
  if (common_len == 0) {
    *err = "one or more tracks are empty";
    return false;
  }
  c->sample_rate_hz = mic_w.sample_rate_hz;
  c->mic            = std::move(mic_w.samples);
  c->ref            = std::move(ref_w.samples);
  c->echo_only_mic  = std::move(echo_w.samples);
  c->mic.resize(common_len);
  c->ref.resize(common_len);
  c->echo_only_mic.resize(common_len);

  const auto near_path = dir / "near_end_clean.wav";
  if (std::filesystem::exists(near_path)) {
    ecnr::hal::WavData near_w;
    if (!ecnr::hal::ReadWavMono(near_path.string(), &near_w, err)) {
      *err = "read near_end_clean.wav: " + *err;
      return false;
    }
    if (near_w.sample_rate_hz != c->sample_rate_hz) {
      *err = "near_end_clean.wav sample-rate mismatch";
      return false;
    }
    c->near_end_clean = std::move(near_w.samples);
    c->near_end_clean.resize(common_len);
    c->has_near_end_clean = true;
  }
  return true;
}

int RunSweep(const std::string& conditions_dir, const std::string& out_csv,
             const std::string& out_wavs_dir,
             const std::string& stage_wavs_dir,
             const ChainConfig& config) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(conditions_dir, ec)) {
    std::fprintf(stderr, "--conditions %s is not a directory\n",
                 conditions_dir.c_str());
    return 1;
  }
  std::vector<fs::path> condition_dirs;
  for (const auto& entry : fs::directory_iterator(conditions_dir, ec)) {
    if (entry.is_directory()) condition_dirs.push_back(entry.path());
  }
  std::sort(condition_dirs.begin(), condition_dirs.end());
  if (condition_dirs.empty()) {
    std::fprintf(stderr, "--conditions %s has no subdirectories\n",
                 conditions_dir.c_str());
    return 1;
  }

  std::ofstream csv(out_csv);
  if (!csv) {
    std::fprintf(stderr, "cannot open --out %s for writing\n", out_csv.c_str());
    return 1;
  }
  csv << "condition_id,config_name,sample_rate_hz,"
         "frame_duration_ms,rtf,"
         "erle_reported_median_db,erle_reported_p10_db,erle_reported_p90_db,"
         "erle_true_median_db,erle_true_p10_db,erle_true_p90_db,"
         "erle_initial_0ms_db,erle_initial_200ms_db,erle_initial_1000ms_db,"
         "erle_initial_1200ms_db,erle_initial_1500ms_db,erle_initial_5000ms_db,"
         "erle_steady_median_db,erle_worst_1s_median_after_settle_db,"
         "erle_time_variation_db,noise_level_range_db,"
         "residual_echo_likelihood_median,residual_echo_likelihood_p90,"
         "near_end_level_delta_median_db,near_end_correlation,near_end_frames_used,"
         "frames_used_true,frames_below_gate_true,frames_skipped_settle_true\n";

  int failures = 0;
  for (const auto& dir : condition_dirs) {
    Condition cond;
    std::string err;
    if (!LoadCondition(dir, &cond, &err)) {
      std::fprintf(stderr, "condition %s: SKIP (%s)\n",
                   dir.filename().string().c_str(), err.c_str());
      ++failures;
      continue;
    }
    std::printf("condition %s: %d samples @ %d Hz\n",
                cond.id.c_str(),
                static_cast<int>(cond.mic.size()),
                cond.sample_rate_hz);

    // Pass A: production-style run on the mixed mic, capture per-frame
    // reported ERLE.
    auto run_a = RunChain(cond.ref, cond.mic, cond.sample_rate_hz,
                          /*num_mics=*/2, config,
                          /*collect_stage_taps=*/!stage_wavs_dir.empty());
    const auto reported =
        AggregatePercentiles(run_a.reported_erle_db_per_frame, kSettleFrames);
    const auto residual_echo_likelihood =
        AggregatePercentiles(run_a.residual_echo_likelihood_per_frame,
                             kSettleFrames);

    // Optionally emit the per-condition chain output WAV. Consumed by
    // reference/score_mos.py to produce DNSMOS / AECMOS scores per
    // ADR-0012 §4 workflow.
    if (!out_wavs_dir.empty()) {
      std::string emsg;
      const fs::path out_wav_path = fs::path(out_wavs_dir) / (cond.id + ".wav");
      fs::create_directories(out_wav_path.parent_path(), ec);
      if (!ecnr::hal::WriteWavMono(out_wav_path.string(), run_a.residual,
                                    cond.sample_rate_hz, &emsg)) {
        std::fprintf(stderr, "WARN: failed to write %s: %s\n",
                     out_wav_path.string().c_str(), emsg.c_str());
      }
    }
    if (!stage_wavs_dir.empty()) {
      const fs::path stage_dir = fs::path(stage_wavs_dir) / cond.id;
      fs::create_directories(stage_dir, ec);
      struct StageOut {
        const char* name;
        const std::vector<int16_t>* samples;
      };
      const StageOut stages[] = {
          {"post_bf", &run_a.post_beamformer},
          {"post_aec", &run_a.post_aec},
          {"post_ns", &run_a.post_ns},
          {"post_agc", &run_a.post_agc},
      };
      for (const auto& stage : stages) {
        std::string emsg;
        const fs::path path = stage_dir / (std::string(stage.name) + ".wav");
        if (!ecnr::hal::WriteWavMono(path.string(), *stage.samples,
                                      cond.sample_rate_hz, &emsg)) {
          std::fprintf(stderr, "WARN: failed to write %s: %s\n",
                       path.string().c_str(), emsg.c_str());
        }
      }
    }

    // Pass B: echo-only run, fresh chain instance, capture residual for
    // true-ERLE.
    ChainConfig echo_metric_config = config;
    echo_metric_config.agc_enabled = false;
    auto run_b = RunChain(cond.ref, cond.echo_only_mic,
                          cond.sample_rate_hz, /*num_mics=*/2,
                          echo_metric_config);
    const auto true_stats =
        ComputeTrueErle(cond.echo_only_mic, run_b.residual, cond.sample_rate_hz);
    const auto true_erle_per_frame =
        ComputePerFrameErle(cond.echo_only_mic, run_b.residual, cond.sample_rate_hz);
    const auto steady_erle = MedianErleInRange(
        true_erle_per_frame,
        std::max(0, static_cast<int>(cond.echo_only_mic.size()) * 1000 /
                        cond.sample_rate_hz - 1000),
        static_cast<int>(cond.echo_only_mic.size()) * 1000 / cond.sample_rate_hz);
    const auto worst_1s = ErleWorstOneSecondMedianAfterSettle(true_erle_per_frame);
    std::optional<double> time_variation;
    if (steady_erle.has_value() && worst_1s.has_value()) {
      time_variation = std::max(0.0, *steady_erle - *worst_1s);
    }
    const auto noise_range =
        LevelRangeDb(run_a.residual, cond.sample_rate_hz,
                     /*window_ms=*/100, /*skip_ms=*/1000);
    NearEndStats near_stats;
    if (cond.has_near_end_clean) {
      near_stats = ComputeNearEndStats(cond.near_end_clean, run_a.residual,
                                       cond.sample_rate_hz);
    }

    auto fmt = [](const std::optional<double>& v) {
      char buf[32];
      if (v.has_value()) std::snprintf(buf, sizeof(buf), "%.3f", *v);
      else               std::snprintf(buf, sizeof(buf), "");
      return std::string(buf);
    };
    csv << cond.id << ","
        << ConfigName(config) << ","
        << cond.sample_rate_hz << ","
        << ecnr::kFrameDurationMs << ","
        << fmt(run_a.rtf) << ","
        << fmt(reported.median_db) << "," << fmt(reported.p10_db) << ","
        << fmt(reported.p90_db) << ","
        << fmt(true_stats.median_db) << "," << fmt(true_stats.p10_db) << ","
        << fmt(true_stats.p90_db) << ","
        << fmt(ErleAtMs(true_erle_per_frame, 0)) << ","
        << fmt(ErleAtMs(true_erle_per_frame, 200)) << ","
        << fmt(ErleAtMs(true_erle_per_frame, 1000)) << ","
        << fmt(ErleAtMs(true_erle_per_frame, 1200)) << ","
        << fmt(ErleAtMs(true_erle_per_frame, 1500)) << ","
        << fmt(ErleAtMs(true_erle_per_frame, 5000)) << ","
        << fmt(steady_erle) << ","
        << fmt(worst_1s) << ","
        << fmt(time_variation) << ","
        << fmt(noise_range) << ","
        << fmt(residual_echo_likelihood.median_db) << ","
        << fmt(residual_echo_likelihood.p90_db) << ","
        << fmt(near_stats.level_delta_median_db) << ","
        << fmt(near_stats.correlation) << ","
        << near_stats.frames_used << ","
        << true_stats.frames_used << ","
        << true_stats.frames_below_gate << ","
        << true_stats.frames_skipped_settle
        << "\n";
  }
  csv.close();
  std::printf("wrote %s (%zu conditions processed, %d skipped)\n",
              out_csv.c_str(), condition_dirs.size() - failures, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (args.self_test) return RunSelfTest();
  ChainConfig config;
  config.agc_enabled = args.agc_enabled;
  config.ns_dry_blend = args.ns_dry_blend;
  config.ns_vad_blend_set = args.ns_vad_blend_set;
  config.ns_vad_blend_low = args.ns_vad_blend_low;
  config.ns_vad_blend_high = args.ns_vad_blend_high;
  return RunSweep(args.conditions_dir, args.out_csv, args.out_wavs_dir,
                  args.stage_wavs_dir, config);
}

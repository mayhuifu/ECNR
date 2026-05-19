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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
  bool agc_enabled = false;  // optional post-NS AGC2 (mirrors ecnr_bench --agc)
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
      "usage: %s --self-test\n"
      "       %s --run --conditions DIR --out FILE.csv [--out-wavs WAV_DIR]\n"
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
      "                        reference/score_mos.py.\n",
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
    else if (flag == "--agc") a->agc_enabled = true;
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
  std::vector<double> reported_erle_db_per_frame;  // unfilled frames omitted
};

ChainRunResult RunChain(const std::vector<int16_t>& ref,
                        const std::vector<int16_t>& capture,
                        int sample_rate_hz, int num_mics,
                        bool agc_enabled = false) {
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
  chain.SetAgcEnabled(agc_enabled);

  const size_t n_frames =
      std::min(ref.size(), capture.size()) /
      static_cast<size_t>(frame_samples);

  ChainRunResult r;
  r.residual.reserve(n_frames * static_cast<size_t>(frame_samples));
  r.reported_erle_db_per_frame.reserve(n_frames);

  ecnr::Frame ref_f, mic_f, out_f;
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
    chain.ProcessCapture(mic_f, out_f);

    r.residual.insert(r.residual.end(), out_f.ch[0].begin(),
                      out_f.ch[0].begin() + out_f.n_samples);

    const auto& s = chain.Stats();
    if (s.echo_return_loss_enhancement_db.has_value()) {
      r.reported_erle_db_per_frame.push_back(*s.echo_return_loss_enhancement_db);
    }
  }
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
  return true;
}

int RunSweep(const std::string& conditions_dir, const std::string& out_csv,
             const std::string& out_wavs_dir, bool agc_enabled) {
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
         "erle_reported_median_db,erle_reported_p10_db,erle_reported_p90_db,"
         "erle_true_median_db,erle_true_p10_db,erle_true_p90_db,"
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
                          /*num_mics=*/2, agc_enabled);
    const auto reported =
        AggregatePercentiles(run_a.reported_erle_db_per_frame, kSettleFrames);

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

    // Pass B: echo-only run, fresh chain instance, capture residual for
    // true-ERLE.
    auto run_b = RunChain(cond.ref, cond.echo_only_mic,
                          cond.sample_rate_hz, /*num_mics=*/2);
    const auto true_stats =
        ComputeTrueErle(cond.echo_only_mic, run_b.residual, cond.sample_rate_hz);

    auto fmt = [](const std::optional<double>& v) {
      char buf[32];
      if (v.has_value()) std::snprintf(buf, sizeof(buf), "%.3f", *v);
      else               std::snprintf(buf, sizeof(buf), "");
      return std::string(buf);
    };
    csv << cond.id << ","
        << "default-webrtc" << ","
        << cond.sample_rate_hz << ","
        << fmt(reported.median_db) << "," << fmt(reported.p10_db) << ","
        << fmt(reported.p90_db) << ","
        << fmt(true_stats.median_db) << "," << fmt(true_stats.p10_db) << ","
        << fmt(true_stats.p90_db) << ","
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
  return RunSweep(args.conditions_dir, args.out_csv, args.out_wavs_dir,
                  args.agc_enabled);
}

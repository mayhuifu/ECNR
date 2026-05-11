#include "eval/metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ecnr::eval {
namespace {

constexpr double kInt16FullScale = 32767.0;

// Linear RMS from dBFS reference. rms_dbfs = 20 · log10(rms / fullscale)
//   => rms = fullscale · 10^(dbfs / 20)
double DbfsToLinearRms(double dbfs) {
  return kInt16FullScale * std::pow(10.0, dbfs / 20.0);
}

// Linear-interpolated percentile of a sorted vector. v must be sorted
// ascending. percentile is in [0, 1].
double SortedPercentile(const std::vector<double>& v, double percentile) {
  if (v.empty()) return 0.0;
  if (v.size() == 1) return v[0];
  const double idx = percentile * static_cast<double>(v.size() - 1);
  const auto lo = static_cast<size_t>(std::floor(idx));
  const auto hi = static_cast<size_t>(std::ceil(idx));
  if (lo == hi) return v[lo];
  const double frac = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - frac) + v[hi] * frac;
}

}  // namespace

double Rms(const int16_t* samples, int n) {
  if (n <= 0 || samples == nullptr) return 0.0;
  double acc = 0.0;
  for (int i = 0; i < n; ++i) {
    const double v = static_cast<double>(samples[i]);
    acc += v * v;
  }
  return std::sqrt(acc / static_cast<double>(n));
}

double RmsDbfs(const int16_t* samples, int n) {
  const double rms = Rms(samples, n);
  if (rms <= 0.0) return -std::numeric_limits<double>::infinity();
  return 20.0 * std::log10(rms / kInt16FullScale);
}

std::optional<double> ErleDb(double rms_input, double rms_residual,
                             double cap_db) {
  if (rms_input <= 0.0) return std::nullopt;
  if (rms_residual <= 0.0) return cap_db;
  const double erle = 20.0 * std::log10(rms_input / rms_residual);
  // Clamp to [-cap_db, +cap_db]. Negative ERLE (residual > input)
  // happens when AEC3 makes things worse — surfacing it as-is rather
  // than clamping to 0 keeps the metric honest.
  return std::clamp(erle, -cap_db, cap_db);
}

// -----------------------------------------------------------------------------

ErleAccumulator::ErleAccumulator(int settle_frames, double input_gate_dbfs)
    : settle_frames_(std::max(0, settle_frames)),
      input_gate_rms_(DbfsToLinearRms(input_gate_dbfs)) {}

void ErleAccumulator::Push(const int16_t* echo_in, const int16_t* residual,
                           int n) {
  // Length mismatch or empty input → drop, but still bump frames_pushed
  // so the caller can detect issues from FramesPushed() vs frames_used.
  if (n <= 0 || echo_in == nullptr || residual == nullptr) {
    ++frames_pushed_;
    return;
  }
  ++frames_pushed_;

  if (frames_pushed_ <= settle_frames_) {
    ++frames_skipped_settle_;
    return;
  }

  const double rms_in = Rms(echo_in, n);
  if (rms_in < input_gate_rms_) {
    ++frames_below_gate_;
    return;
  }
  const double rms_res = Rms(residual, n);
  if (auto erle = ErleDb(rms_in, rms_res); erle.has_value()) {
    erle_per_frame_db_.push_back(*erle);
  }
}

ErleStats ErleAccumulator::Finalize() const {
  ErleStats s;
  s.frames_skipped_settle = frames_skipped_settle_;
  s.frames_below_gate = frames_below_gate_;
  s.frames_used = static_cast<int>(erle_per_frame_db_.size());
  if (erle_per_frame_db_.empty()) return s;
  std::vector<double> sorted = erle_per_frame_db_;
  std::sort(sorted.begin(), sorted.end());
  s.median_db = SortedPercentile(sorted, 0.50);
  s.p10_db    = SortedPercentile(sorted, 0.10);
  s.p90_db    = SortedPercentile(sorted, 0.90);
  return s;
}

}  // namespace ecnr::eval

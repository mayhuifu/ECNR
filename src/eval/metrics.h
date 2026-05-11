#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ecnr::eval {

// Per-frame ERLE samples and their distribution. ERLE per frame is
//   20 · log10(rms(echo_in_frame) / rms(echo_residual_frame))
// computed only on frames where rms(echo_in_frame) exceeds an input-
// energy gate (default -50 dBFS). Frames below the gate are dropped —
// they're silent echo windows that would otherwise produce noise-floor-
// dominated ratios.
//
// All percentiles are reported in dB. nullopt fields mean the run had
// no valid frames (e.g., entirely sub-gate input — caller should treat
// as a fixture issue, not a chain bug).
struct ErleStats {
  // Number of frames considered (post-settle and post-gate).
  int frames_used = 0;
  // Number of frames dropped because echo_in RMS was below the input-
  // energy gate.
  int frames_below_gate = 0;
  // Number of frames dropped as part of the AEC3 settle window.
  int frames_skipped_settle = 0;

  // Median (p50), p10, p90 of per-frame ERLE in dB.
  std::optional<double> median_db;
  std::optional<double> p10_db;
  std::optional<double> p90_db;
};

// RMS amplitude of an int16 buffer over its full extent. Returns 0 for
// empty input. Pure function, no allocations.
double Rms(const int16_t* samples, int n);

// RMS, expressed as dBFS relative to int16 full-scale (32767). Useful
// for the input-energy gate. Returns -infinity for digital silence.
double RmsDbfs(const int16_t* samples, int n);

// Compute ERLE in dB given input + residual RMS values. Returns the
// large-but-bounded value `cap_db` when the residual is exactly zero
// (perfect cancellation; otherwise we'd return +inf, which is hard for
// callers to handle). Returns nullopt when rms_input <= 0 (caller's bug).
std::optional<double> ErleDb(double rms_input, double rms_residual,
                             double cap_db = 80.0);

// Pure-data accumulator. Caller pushes one frame at a time via Push();
// Finalize() returns the distribution. Settle / gate semantics:
//
//   - settle_frames: the first `settle_frames` frames are unconditionally
//     dropped (AEC3 convergence transient). Default 100 (= 1 s at 10 ms
//     frames), matching ADR-0011 §4 "settle window".
//
//   - input_gate_dbfs: frames with rms(echo_in) < input_gate_dbfs are
//     dropped from the percentile computation but counted in
//     frames_below_gate. Default -50 dBFS (one bit above near-digital-
//     silence at 16 bit). Set to -120 to effectively disable.
class ErleAccumulator {
 public:
  ErleAccumulator(int settle_frames = 100, double input_gate_dbfs = -50.0);

  // Push one matched (echo_in, residual) frame pair. Both must be the
  // same length; mismatched lengths produce a frame drop without crash.
  void Push(const int16_t* echo_in, const int16_t* residual, int n);

  ErleStats Finalize() const;

  // Total frames pushed (for sanity-checking by the caller).
  int FramesPushed() const { return frames_pushed_; }

 private:
  int settle_frames_;
  double input_gate_rms_;  // linear scale, derived from dBFS at construction
  int frames_pushed_ = 0;
  int frames_skipped_settle_ = 0;
  int frames_below_gate_ = 0;
  std::vector<double> erle_per_frame_db_;
};

}  // namespace ecnr::eval

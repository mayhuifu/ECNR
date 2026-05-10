#pragma once

#include <array>

#include "core/frame.h"  // for kMaxMics

namespace ecnr {

// Cartesian mic-array geometry hint passed to Beamformer::Init.
//
// Frame of reference: arbitrary, but consistent — typically the cabin's
// "driver-forward" frame, with +x = forward (toward windshield), +y = left
// (toward driver in LHD), +z = up. Units: meters. Origin is anywhere
// convenient (often the geometric centroid of the mic array).
//
// Only the first num_mics entries of positions_m are read; the rest are
// ignored. num_mics must match Beamformer::Init(... , num_mics, ...).
//
// Per ADR-0010: this struct is intentionally minimal. Per-mic gain
// calibration, multiple constraint directions (LCMV), and noise-field
// hints are deliberately omitted; they are backward-compatible field adds
// when MVDR / GSC / LCMV land in Phase 1.5+.
struct MicGeometry {
  // (x, y, z) in meters for each mic, in the same channel order as
  // Frame::ch[c]. ch[0] is the reference mic by convention; the DSB
  // implementation does not require it to be the zero-delay channel, but
  // callers are encouraged to keep that convention so logs/diagnostics
  // stay readable.
  std::array<std::array<float, 3>, kMaxMics> positions_m{};

  // Unit vector pointing from the mic array toward the source of interest
  // (e.g., the driver's mouth). Default: +x (forward). The beamformer
  // steers a beam in this direction; off-direction signals are attenuated.
  std::array<float, 3> target_direction{1.0f, 0.0f, 0.0f};

  // Speed of sound, m/s. Default: 343 (20 C, dry air). Cabin temperature
  // varies; precise value is not load-bearing for DSB at typical mic
  // spacings (sub-sample delay error at 16 kHz / 4 cm / +-20 C).
  float speed_of_sound_mps = 343.0f;
};

// Convenience: a "no spatial information" geometry. Beamformer::Init with
// this falls back to selecting ch[0] (Phase 0.5 stub behaviour). Useful for
// HAL bring-up, unit tests, and the bench/live --bypass-beamformer mode.
constexpr MicGeometry kPassthroughGeometry{};

}  // namespace ecnr

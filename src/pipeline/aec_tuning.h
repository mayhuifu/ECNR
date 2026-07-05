#pragma once

#include <cstdlib>
#include <string>

namespace ecnr {

// AEC3 double-talk transparency tuning (GB/T 45314 §5.7, ADR-0013).
//
// WebRTC AEC3's suppressor ships conferencing-tuned: during double-talk it
// prefers killing residual echo over preserving a quiet near-end talker.
// GB/T 45314's emergency-call double-talk clause (§5.7, send-side grade 2b)
// wants the opposite trade for a driver speaking 6 dB below the downlink
// echo. These knobs map 1:1 onto EchoCanceller3Config::Suppressor fields —
// the dominant-nearend *detector* (when the suppressor believes the near end
// is talking) and the nearend *tuning* masks (how transparent it goes when
// it does). Echo-clause floors (TCL, convergence) arbitrate how far they
// can be pushed; values land in ADR-0013 once swept.
//
// Sentinel semantics: negative value = leave WebRTC's default untouched.
// Must be applied BEFORE AecChain::Init (baked into the EchoControlFactory
// at APM construction, same contract as SetAecFilterLengthBlocks).
struct AecDtTuning {
  // EchoCanceller3Config::Suppressor::DominantNearendDetection
  float nearend_enr_threshold = -1.0f;      // default .25 — echo-to-nearend
                                            // ratio below which a band counts
                                            // as nearend; higher = detector
                                            // triggers with more echo present
  float nearend_snr_threshold = -1.0f;      // default 30 — nearend-to-noise
                                            // ratio required; lower = triggers
                                            // for quieter talkers
  int nearend_hold_duration = -1;           // default 50 blocks (4 ms each)
  int nearend_trigger_threshold = -1;       // default 12 consecutive blocks
  // EchoCanceller3Config::Suppressor::nearend_tuning
  float mask_lf_enr_transparent = -1.0f;    // default 1.09
  float mask_lf_enr_suppress = -1.0f;       // default 1.1
  float mask_hf_enr_transparent = -1.0f;    // default .1 — HF bands (the
                                            // consonant range correlation
                                            // lives on) suppress far earlier
                                            // than LF by default
  float mask_hf_enr_suppress = -1.0f;       // default .3
  float max_dec_factor_lf = -1.0f;          // default .25 — how fast gains
                                            // may fall between blocks

  bool Any() const {
    return nearend_enr_threshold >= 0.0f || nearend_snr_threshold >= 0.0f ||
           nearend_hold_duration >= 0 || nearend_trigger_threshold >= 0 ||
           mask_lf_enr_transparent >= 0.0f || mask_lf_enr_suppress >= 0.0f ||
           mask_hf_enr_transparent >= 0.0f || mask_hf_enr_suppress >= 0.0f ||
           max_dec_factor_lf >= 0.0f;
  }
};

// Parses "k=v[,k=v...]" specs from the --aec3-tune CLI flag shared by
// ecnr_eval and ecnr_bench. Keys: enr, snr, hold, trigger, mask_t, mask_s,
// dec_lf. Returns false (with *err set) on unknown key or unparsable value.
inline bool ParseAecDtTuning(const std::string& spec, AecDtTuning* out,
                             std::string* err) {
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    const std::string pair = spec.substr(pos, comma - pos);
    pos = comma + 1;
    if (pair.empty()) continue;
    const size_t eq = pair.find('=');
    if (eq == std::string::npos) {
      *err = "expected k=v, got '" + pair + "'";
      return false;
    }
    const std::string key = pair.substr(0, eq);
    const std::string val = pair.substr(eq + 1);
    char* end = nullptr;
    const float f = std::strtof(val.c_str(), &end);
    if (end == val.c_str() || *end != '\0') {
      *err = "could not parse value '" + val + "' for key '" + key + "'";
      return false;
    }
    if      (key == "enr")     out->nearend_enr_threshold = f;
    else if (key == "snr")     out->nearend_snr_threshold = f;
    else if (key == "hold")    out->nearend_hold_duration = static_cast<int>(f);
    else if (key == "trigger") out->nearend_trigger_threshold = static_cast<int>(f);
    else if (key == "mask_t")  out->mask_lf_enr_transparent = f;
    else if (key == "mask_s")  out->mask_lf_enr_suppress = f;
    else if (key == "mask_hf_t") out->mask_hf_enr_transparent = f;
    else if (key == "mask_hf_s") out->mask_hf_enr_suppress = f;
    else if (key == "dec_lf")  out->max_dec_factor_lf = f;
    else {
      *err = "unknown key '" + key +
             "' (expected enr|snr|hold|trigger|mask_t|mask_s|"
             "mask_hf_t|mask_hf_s|dec_lf)";
      return false;
    }
  }
  return true;
}

}  // namespace ecnr

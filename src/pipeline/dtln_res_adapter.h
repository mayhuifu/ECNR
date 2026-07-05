#pragma once

#include <memory>
#include <string>

#include "core/frame.h"

namespace ecnr {

// DTLN-AEC neural echo-control branch (Phase 3 RES, ADR-0014).
//
// Runs the vendored DTLN-AEC model (two stacked stateful-LSTM stages,
// 512-sample blocks with 128-sample hop, 16 kHz) over the RAW mic signal
// plus the loopback/render reference, via ONNX Runtime. This is the "B
// path" of the parallel hybrid: it preserves a near-end talker sitting
// well below the echo (the case AEC3's suppressor cannot win — see
// docs/phase-3-res-hybrid-notes.md) but only attenuates far-end echo by
// ~16 dB on its own, so AecChain fuses it per-frame with the AEC3 path
// using the echo likelihood computed here.
//
// Latency: exactly kLatencySamples (384 = block − hop, 24 ms at 16 kHz)
// relative to the pushed input — the output FIFO is primed so every
// Process() call yields a full frame. AecChain delays the A path by the
// same amount before fusing.
//
// 16 kHz only (eCall is WB; the 48 kHz tier has no RES stage yet —
// Init() rejects other rates). All ONNX Runtime types stay inside the
// pimpl; this header is vendor-clean like the other adapters.
class DtlnResAdapter {
 public:
  static constexpr int kLatencySamples16k = 384;

  DtlnResAdapter();
  ~DtlnResAdapter();

  DtlnResAdapter(const DtlnResAdapter&) = delete;
  DtlnResAdapter& operator=(const DtlnResAdapter&) = delete;

  // Loads models/dtln_aec_<units>_{1,2}.onnx from model_dir. Returns false
  // on unsupported rate (only 16000), missing files, or ORT session
  // failure (error to stderr).
  bool Init(int sample_rate_hz, const std::string& model_dir, int units);

  // Clears LSTM states, FIFOs, and the likelihood tracker. Not real-time
  // safe (reallocates ORT value buffers); call between streams only.
  void Reset();

  // Feed the render/loopback frame for the same 10 ms tick as the next
  // ProcessCapture. Mono, chain rate.
  void PushRender(const Frame& render);

  // Process a raw-mic mono frame; writes the B-path output (delayed by
  // kLatencySamples16k) into out. Returns false (out = silence) before
  // Init or on shape mismatch.
  bool ProcessCapture(const Frame& mic_in, Frame& out);

  // Echo likelihood of the CURRENT B-path output frame in [0, 1]:
  // max lagged correlation between the render and B-output log-power
  // sequences over the trailing window. High = output is render-driven
  // (echo residual, select the AEC3 path); low = independent near-end
  // speech (select this path). Valid after each ProcessCapture.
  float LastEchoLikelihood() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ecnr

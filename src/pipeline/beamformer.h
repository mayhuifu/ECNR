#pragma once

#include "core/frame.h"

namespace ecnr {

// Phase 0.5 stub: collapses N-channel input to mono by selecting ch[0].
// Phase 1+ replaces with real beamforming (delay-and-sum / MVDR / GSC) per
// ADR-0010 (tbd). The interface is locked now so AEC3 always sees mono.
class Beamformer {
 public:
  Beamformer();
  ~Beamformer();

  Beamformer(const Beamformer&) = delete;
  Beamformer& operator=(const Beamformer&) = delete;

  // Initialize for a specific (rate, mic_count). Both must be supported
  // (see IsSupportedSampleRate, IsSupportedMicCount). Returns false otherwise.
  // TODO(ADR-0010): real beamformer (MVDR/GSC) will need a geometry hint
  // (mic spacing or coords). Add as a separate setter or an extended Init
  // overload to avoid breaking this signature.
  bool Init(int sample_rate_hz, int num_mics);

  // multi -> mono. Asserts mic_in.n_channels == configured num_mics and
  // mic_in.n_samples == configured frame samples. mono_out is set to
  // n_channels=1 with samples drawn from ch[0] (stub) or beamformed (future).
  void Process(const Frame& mic_in, Frame& mono_out);

  void Reset();

 private:
  int sample_rate_hz_ = 0;
  int num_mics_ = 0;
};

}  // namespace ecnr

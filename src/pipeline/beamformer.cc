#include "pipeline/beamformer.h"

#include <cassert>
#include <cstdio>

#include "core/frame.h"

namespace ecnr {

Beamformer::Beamformer() = default;
Beamformer::~Beamformer() = default;

bool Beamformer::Init(int sample_rate_hz, int num_mics) {
  if (!IsSupportedSampleRate(sample_rate_hz)) return false;
  if (!IsSupportedMicCount(num_mics)) return false;
  sample_rate_hz_ = sample_rate_hz;
  num_mics_ = num_mics;
  return true;
}

void Beamformer::Process(const Frame& mic_in, Frame& mono_out) {
  assert(mic_in.n_channels == num_mics_);
  assert(mic_in.n_samples == FrameSamplesFor(sample_rate_hz_));
  if (mic_in.n_channels != num_mics_ ||
      mic_in.n_samples != FrameSamplesFor(sample_rate_hz_)) {
    std::fprintf(stderr,
        "Beamformer::Process: dropping frame (n_channels=%d expected=%d, n_samples=%d expected=%d)\n",
        mic_in.n_channels, num_mics_, mic_in.n_samples,
        FrameSamplesFor(sample_rate_hz_));
    mono_out.n_channels = 1;
    mono_out.n_samples = 0;  // signal: invalid output
    return;
  }

  // Phase 0.5 stub: select ch[0] verbatim. Real beamforming (delay-and-sum /
  // MVDR / GSC) lands in Phase 1+ behind ADR-0010.
  mono_out.n_channels = 1;
  mono_out.n_samples = mic_in.n_samples;
  for (int s = 0; s < mic_in.n_samples; ++s) {
    mono_out.ch[0][s] = mic_in.ch[0][s];
  }
}

void Beamformer::Reset() {
  // No adaptive state in the stub. Real beamformer will clear filter taps,
  // VAD history, etc.
}

}  // namespace ecnr

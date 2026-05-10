#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ecnr::hal {

struct WavData {
  std::vector<int16_t> samples;  // interleaved if multi-channel
  int sample_rate_hz = 0;
  int channels = 0;
};

// Reads a WAV file and downmixes to mono if needed. Returns false on error
// and sets *err. The chain expects mono int16; non-int16 inputs are converted.
bool ReadWavMono(const std::string& path, WavData* out, std::string* err);

// Writes a mono int16 WAV at sample_rate_hz. Returns false on error.
bool WriteWavMono(const std::string& path, const std::vector<int16_t>& samples,
                  int sample_rate_hz, std::string* err);

}  // namespace ecnr::hal

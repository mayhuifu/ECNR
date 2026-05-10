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

// Reads a WAV file preserving its channel layout. `out->samples` is the
// interleaved int16 stream as stored in the file; `out->channels` is the
// actual file channel count. Used by ecnr_bench to ingest real multi-mic
// captures and route them straight into Frame::ch[c] (ADR-0010 §5
// "option A"). Returns false on error and sets *err.
bool ReadWav(const std::string& path, WavData* out, std::string* err);

// Writes a mono int16 WAV at sample_rate_hz. Returns false on error.
bool WriteWavMono(const std::string& path, const std::vector<int16_t>& samples,
                  int sample_rate_hz, std::string* err);

}  // namespace ecnr::hal

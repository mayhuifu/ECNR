#include "hal/file_io.h"

#include <sndfile.h>

#include <cstring>

namespace ecnr::hal {

bool ReadWavMono(const std::string& path, WavData* out, std::string* err) {
  SF_INFO info{};
  SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
  if (!sf) {
    if (err) *err = std::string("sf_open: ") + sf_strerror(nullptr);
    return false;
  }
  std::vector<int16_t> interleaved(static_cast<size_t>(info.frames) * info.channels);
  const sf_count_t got = sf_readf_short(sf, interleaved.data(), info.frames);
  sf_close(sf);
  if (got != info.frames) {
    if (err) *err = "sf_readf_short returned short count";
    return false;
  }

  out->sample_rate_hz = info.samplerate;
  out->channels = 1;
  if (info.channels == 1) {
    out->samples = std::move(interleaved);
  } else {
    out->samples.resize(info.frames);
    for (sf_count_t i = 0; i < info.frames; ++i) {
      int32_t acc = 0;
      for (int c = 0; c < info.channels; ++c) {
        acc += interleaved[i * info.channels + c];
      }
      out->samples[i] = static_cast<int16_t>(acc / info.channels);
    }
  }
  return true;
}

bool ReadWav(const std::string& path, WavData* out, std::string* err) {
  SF_INFO info{};
  SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
  if (!sf) {
    if (err) *err = std::string("sf_open: ") + sf_strerror(nullptr);
    return false;
  }
  out->samples.resize(static_cast<size_t>(info.frames) * info.channels);
  const sf_count_t got = sf_readf_short(sf, out->samples.data(), info.frames);
  sf_close(sf);
  if (got != info.frames) {
    if (err) *err = "sf_readf_short returned short count";
    return false;
  }
  out->sample_rate_hz = info.samplerate;
  out->channels = info.channels;
  return true;
}

bool WriteWavMono(const std::string& path, const std::vector<int16_t>& samples,
                  int sample_rate_hz, std::string* err) {
  SF_INFO info{};
  info.samplerate = sample_rate_hz;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!sf) {
    if (err) *err = std::string("sf_open(write): ") + sf_strerror(nullptr);
    return false;
  }
  const sf_count_t wrote =
      sf_writef_short(sf, samples.data(), static_cast<sf_count_t>(samples.size()));
  sf_close(sf);
  if (wrote != static_cast<sf_count_t>(samples.size())) {
    if (err) *err = "sf_writef_short returned short count";
    return false;
  }
  return true;
}

}  // namespace ecnr::hal

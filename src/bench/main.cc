#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/frame.h"
#include "hal/file_io.h"
#include "pipeline/aec_chain.h"

namespace {

struct Args {
  std::string mic;
  std::string ref;
  std::string out = "out.wav";
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s --mic mic.wav --ref ref.wav [--out out.wav]\n", prog);
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if ((flag == "--mic" || flag == "--ref" || flag == "--out") && i + 1 < argc) {
      const std::string val = argv[++i];
      if (flag == "--mic") a->mic = val;
      if (flag == "--ref") a->ref = val;
      if (flag == "--out") a->out = val;
    } else if (flag == "-h" || flag == "--help") {
      return false;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", flag.c_str());
      return false;
    }
  }
  return !a->mic.empty() && !a->ref.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::string err;
  ecnr::hal::WavData mic_wav, ref_wav;
  if (!ecnr::hal::ReadWavMono(args.mic, &mic_wav, &err)) {
    std::fprintf(stderr, "read mic: %s\n", err.c_str());
    return 1;
  }
  if (!ecnr::hal::ReadWavMono(args.ref, &ref_wav, &err)) {
    std::fprintf(stderr, "read ref: %s\n", err.c_str());
    return 1;
  }
  if (mic_wav.sample_rate_hz != ecnr::kSampleRateHz ||
      ref_wav.sample_rate_hz != ecnr::kSampleRateHz) {
    std::fprintf(stderr,
                 "Phase 0 expects %d Hz inputs (got mic=%d, ref=%d). "
                 "Resampling will land in Phase 0.5.\n",
                 ecnr::kSampleRateHz, mic_wav.sample_rate_hz,
                 ref_wav.sample_rate_hz);
    return 1;
  }

  ecnr::AecChain chain;
  if (!chain.Init(ecnr::kSampleRateHz)) {
    std::fprintf(stderr, "chain init failed\n");
    return 1;
  }

  const size_t total_frames =
      std::min(mic_wav.samples.size(), ref_wav.samples.size()) /
      ecnr::kFrameSamples;
  std::vector<int16_t> processed;
  processed.reserve(total_frames * ecnr::kFrameSamples);

  ecnr::Frame mic_f, ref_f, out_f;
  for (size_t i = 0; i < total_frames; ++i) {
    const size_t off = i * ecnr::kFrameSamples;
    std::memcpy(ref_f.samples.data(), ref_wav.samples.data() + off,
                ecnr::kFrameSamples * sizeof(int16_t));
    std::memcpy(mic_f.samples.data(), mic_wav.samples.data() + off,
                ecnr::kFrameSamples * sizeof(int16_t));
    chain.ProcessRender(ref_f);
    chain.ProcessCapture(mic_f, out_f);
    processed.insert(processed.end(), out_f.samples.begin(), out_f.samples.end());
  }

  if (!ecnr::hal::WriteWavMono(args.out, processed, ecnr::kSampleRateHz, &err)) {
    std::fprintf(stderr, "write out: %s\n", err.c_str());
    return 1;
  }

  const auto& s = chain.Stats();
  // erle_db is sourced from APM stats (Task 6); under the Phase-0 stub the
  // optional is nullopt and this prints 0.00.
  std::printf("frames=%zu  audio=%.3fs  cpu=%.3fs  rtf=%.4f  "
              "erle_db=%.2f (stub: 0)\n",
              total_frames, s.audio_time_s, s.cpu_time_s, s.Rtf(),
              s.echo_return_loss_enhancement_db.value_or(0.0));
  return 0;
}

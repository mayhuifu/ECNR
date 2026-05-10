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
  // 0 = unchanged RNNoise (default); 0.25 caps suppression at ~-12 dB; 1 = NS
  // bypass. See AecChain::SetNsDryBlend / RnNsAdapter::SetDryBlend.
  float ns_dry_blend = 0.0f;
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s --mic mic.wav --ref ref.wav [--out out.wav] "
               "[--ns-dry-blend <0..1>]\n", prog);
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if ((flag == "--mic" || flag == "--ref" || flag == "--out") && i + 1 < argc) {
      const std::string val = argv[++i];
      if (flag == "--mic") a->mic = val;
      if (flag == "--ref") a->ref = val;
      if (flag == "--out") a->out = val;
    } else if (flag == "--ns-dry-blend" && i + 1 < argc) {
      a->ns_dry_blend = std::stof(argv[++i]);
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
  if (!ecnr::IsSupportedSampleRate(mic_wav.sample_rate_hz) ||
      mic_wav.sample_rate_hz != ref_wav.sample_rate_hz) {
    std::fprintf(stderr,
                 "ecnr_bench expects 16000 or 48000 Hz mono inputs, both at "
                 "the same rate (got mic=%d, ref=%d).\n",
                 mic_wav.sample_rate_hz, ref_wav.sample_rate_hz);
    return 1;
  }
  const int sample_rate_hz = mic_wav.sample_rate_hz;
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);

  // ADR-0004 fixes the production mic-count contract at [2, 8]; this offline
  // harness has only mono input. We honor the contract by initializing the
  // chain at num_mics = 2 and duplicating the mono signal into ch[0] and
  // ch[1]. The Beamformer stub picks ch[0], so the duplicated channel has
  // no effect on the processed output but keeps the chain interface honest.
  // TODO(ADR-0010): mono-duplicated 2-channel input is degenerate for any real
  // beamformer (zero inter-mic delay; perfectly correlated channels — singular
  // covariance under MVDR). Once the real beamformer lands, this harness should
  // either accept multi-channel WAV input or add a --bypass-beamformer flag.
  ecnr::AecChain chain;
  if (!chain.Init(sample_rate_hz, 2)) {
    std::fprintf(stderr, "chain init failed\n");
    return 1;
  }
  chain.SetNsDryBlend(args.ns_dry_blend);

  const size_t total_frames =
      std::min(mic_wav.samples.size(), ref_wav.samples.size()) /
      static_cast<size_t>(frame_samples);
  std::vector<int16_t> processed;
  processed.reserve(total_frames * frame_samples);

  ecnr::Frame mic_f, ref_f, out_f;
  for (size_t i = 0; i < total_frames; ++i) {
    const size_t off = i * frame_samples;
    // Render is mono.
    ref_f.n_samples = frame_samples;
    ref_f.n_channels = 1;
    std::memcpy(ref_f.ch[0].data(), ref_wav.samples.data() + off,
                frame_samples * sizeof(int16_t));
    // Mic is 2-channel; duplicate the mono input into ch[0] and ch[1].
    mic_f.n_samples = frame_samples;
    mic_f.n_channels = 2;
    std::memcpy(mic_f.ch[0].data(), mic_wav.samples.data() + off,
                frame_samples * sizeof(int16_t));
    std::memcpy(mic_f.ch[1].data(), mic_wav.samples.data() + off,
                frame_samples * sizeof(int16_t));

    chain.ProcessRender(ref_f);
    chain.ProcessCapture(mic_f, out_f);
    processed.insert(processed.end(), out_f.ch[0].begin(),
                     out_f.ch[0].begin() + out_f.n_samples);
  }

  if (!ecnr::hal::WriteWavMono(args.out, processed, sample_rate_hz, &err)) {
    std::fprintf(stderr, "write out: %s\n", err.c_str());
    return 1;
  }

  const auto& s = chain.Stats();
  // erle_db is sourced from APM stats (Task 6); under the Phase-0 stub the
  // optional is nullopt and we print N/A. Once the WebRTC backend lands,
  // the same code path emits the real value without a label change.
  std::printf("frames=%zu  audio=%.3fs  cpu=%.3fs  rtf=%.4f",
              total_frames, s.audio_time_s, s.cpu_time_s, s.Rtf());
  if (s.echo_return_loss_enhancement_db.has_value()) {
    std::printf("  erle_db=%.2f", *s.echo_return_loss_enhancement_db);
  } else {
    std::printf("  erle_db=N/A");
  }
  std::printf("  dropped=%llu",
              static_cast<unsigned long long>(s.frames_dropped));
  if (args.ns_dry_blend > 0.0f) {
    std::printf("  ns_dry_blend=%.3f", args.ns_dry_blend);
  }
  std::printf("\n");
  return 0;
}

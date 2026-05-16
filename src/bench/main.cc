#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/frame.h"
#include "hal/file_io.h"
#include "pipeline/aec_chain.h"
#include "pipeline/mic_geometry.h"

namespace {

struct Args {
  std::string mic;
  std::string ref;
  std::string out = "out.wav";
  // Optional "before" track for A/B listening. When set, the bench writes
  // the pre-chain mic stream (ch[0], sample-aligned with --out) to this
  // path. Same shape as ecnr_live's --out-raw flag. The original mic.wav
  // is usually a few samples longer than --out (the chain drops trailing
  // partial frames); having a length-matched WAV makes direct A/B
  // playback + visual overlay clean.
  std::string out_raw;
  // Step A: uniform blend. 0 = unchanged RNNoise (default); 0.25 caps
  // suppression at ~-12 dB; 1 = NS bypass.
  float ns_dry_blend = 0.0f;
  // Step B: VAD-gated blend range. When both > 0, supersedes ns_dry_blend.
  // low = blend used for noise-dominant frames (typically 0);
  // high = blend used for voice-dominant frames (typically 0.25-0.30).
  bool ns_vad_blend_set = false;
  float ns_vad_blend_low = 0.0f;
  float ns_vad_blend_high = 0.0f;
  // ADR-0010: when true, the chain initialises Beamformer with
  // kPassthroughGeometry (ch[0]-verbatim fast path) — the right choice
  // when the mic input is a 1-channel WAV duplicated into ch[0] and ch[1]
  // (bench's Phase-0.5 default). When false (the new default), DSB is
  // active and feeding bit-identical channels triggers a one-shot
  // stderr warning from Beamformer.
  bool bypass_beamformer = false;
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s --mic mic.wav --ref ref.wav [--out out.wav]\n"
               "       [--out-raw before.wav]\n"
               "       [--ns-dry-blend <0..1>] | [--ns-vad-blend <low,high>]\n"
               "       [--bypass-beamformer]\n"
               "\n"
               "  --out FILE           processed AEC+NS output (default: out.wav)\n"
               "  --out-raw FILE       captures the pre-chain mic stream (ch[0] verbatim,\n"
               "                       length-aligned with --out). One line of afplay\n"
               "                       output is printed on exit when both are set.\n"
               "  --bypass-beamformer  use Beamformer passthrough (ch[0] verbatim)\n"
               "                       instead of DSB. Right for mono input duplicated\n"
               "                       across channels (no spatial information).\n",
               prog);
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if ((flag == "--mic" || flag == "--ref" || flag == "--out" ||
         flag == "--out-raw") && i + 1 < argc) {
      const std::string val = argv[++i];
      if (flag == "--mic") a->mic = val;
      if (flag == "--ref") a->ref = val;
      if (flag == "--out") a->out = val;
      if (flag == "--out-raw") a->out_raw = val;
    } else if (flag == "--ns-dry-blend" && i + 1 < argc) {
      a->ns_dry_blend = std::stof(argv[++i]);
    } else if (flag == "--ns-vad-blend" && i + 1 < argc) {
      const std::string val = argv[++i];
      const auto comma = val.find(',');
      if (comma == std::string::npos) {
        std::fprintf(stderr,
            "--ns-vad-blend expects \"low,high\" (e.g. 0.0,0.30); got: %s\n",
            val.c_str());
        return false;
      }
      try {
        a->ns_vad_blend_low = std::stof(val.substr(0, comma));
        a->ns_vad_blend_high = std::stof(val.substr(comma + 1));
        a->ns_vad_blend_set = true;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "could not parse --ns-vad-blend value '%s': %s\n",
                     val.c_str(), e.what());
        return false;
      }
    } else if (flag == "--bypass-beamformer") {
      a->bypass_beamformer = true;
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
  // mic_wav preserves channel layout (ADR-0010 §5 option A); ref_wav stays
  // mono — render is always single-channel in this chain.
  ecnr::hal::WavData mic_wav, ref_wav;
  if (!ecnr::hal::ReadWav(args.mic, &mic_wav, &err)) {
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
                 "ecnr_bench expects 16000 or 48000 Hz inputs, both at "
                 "the same rate (got mic=%d, ref=%d).\n",
                 mic_wav.sample_rate_hz, ref_wav.sample_rate_hz);
    return 1;
  }
  const int sample_rate_hz = mic_wav.sample_rate_hz;
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);

  // Mic channel layout drives num_mics. A 1-channel file is duplicated into
  // ch[0]+ch[1] (Phase-0.5 default, degenerate for DSB → bypass recommended).
  // A 2..kMaxMics file routes channels directly to Frame::ch[c] (ADR-0010
  // §5 option A). >kMaxMics is rejected.
  const int file_mic_channels = mic_wav.channels;
  int num_mics = 0;
  if (file_mic_channels == 1) {
    num_mics = 2;  // duplicate-into-2 fallback
  } else if (file_mic_channels >= 2 && file_mic_channels <= ecnr::kMaxMics) {
    num_mics = file_mic_channels;
  } else {
    std::fprintf(stderr,
                 "ecnr_bench: mic WAV has %d channels; expected 1 (duplicated) "
                 "or 2..%d (multi-mic capture).\n",
                 file_mic_channels, ecnr::kMaxMics);
    return 1;
  }

  // Geometry for the DSB path: a uniform linear array along +x, centred on
  // the origin, with 4 cm inter-mic spacing and the target direction at +x.
  // This is a placeholder per ADR-0010 §"Open assumptions" — once a U300
  // recording lands with the real array geometry, callers can pass a
  // matching MicGeometry through AecChain::Init's 3-arg overload.
  constexpr float kPlaceholderSpacingM = 0.04f;
  ecnr::MicGeometry placeholder_geom{};
  for (int c = 0; c < num_mics; ++c) {
    const float x = (static_cast<float>(c) -
                     (static_cast<float>(num_mics) - 1.0f) * 0.5f) *
                    kPlaceholderSpacingM;
    placeholder_geom.positions_m[c] = {x, 0.0f, 0.0f};
  }
  placeholder_geom.target_direction = {1.0f, 0.0f, 0.0f};
  placeholder_geom.speed_of_sound_mps = 343.0f;

  ecnr::AecChain chain;
  const bool chain_ok = args.bypass_beamformer
      ? chain.Init(sample_rate_hz, num_mics)
      : chain.Init(sample_rate_hz, num_mics, placeholder_geom);
  if (!chain_ok) {
    std::fprintf(stderr, "chain init failed (rate=%d, num_mics=%d)\n",
                 sample_rate_hz, num_mics);
    return 1;
  }
  // --ns-vad-blend (Step B) supersedes --ns-dry-blend (Step A) when both
  // are passed; Step A is equivalent to VAD-blend with low == high.
  if (args.ns_vad_blend_set) {
    chain.SetNsVadBlendRange(args.ns_vad_blend_low, args.ns_vad_blend_high);
  } else {
    chain.SetNsDryBlend(args.ns_dry_blend);
  }

  // mic_wav.samples is interleaved; total frame count is bounded by both
  // the mic and ref durations (whichever is shorter wins; render frames
  // beyond either side just pad with zero).
  const size_t mic_frames_available =
      mic_wav.samples.size() / static_cast<size_t>(file_mic_channels);
  const size_t total_frames =
      std::min(mic_frames_available, ref_wav.samples.size()) /
      static_cast<size_t>(frame_samples);
  std::vector<int16_t> processed;
  processed.reserve(total_frames * frame_samples);
  // Optional "before" track. Captures the ch[0] mic samples the chain
  // actually sees (post-de-interleave for multi-channel input), so that
  // out + out_raw are guaranteed sample-aligned for direct A/B.
  std::vector<int16_t> raw;
  if (!args.out_raw.empty()) raw.reserve(total_frames * frame_samples);

  ecnr::Frame mic_f, ref_f, out_f;
  for (size_t i = 0; i < total_frames; ++i) {
    const size_t off = i * frame_samples;
    // Render is mono.
    ref_f.n_samples = frame_samples;
    ref_f.n_channels = 1;
    std::memcpy(ref_f.ch[0].data(), ref_wav.samples.data() + off,
                frame_samples * sizeof(int16_t));
    // Mic: route file channels into Frame::ch[c]. Mono file is duplicated
    // into both channels (Phase-0.5 fallback); multi-channel file is
    // de-interleaved straight into ch[0..N-1].
    mic_f.n_samples = frame_samples;
    mic_f.n_channels = num_mics;
    if (file_mic_channels == 1) {
      std::memcpy(mic_f.ch[0].data(), mic_wav.samples.data() + off,
                  frame_samples * sizeof(int16_t));
      std::memcpy(mic_f.ch[1].data(), mic_wav.samples.data() + off,
                  frame_samples * sizeof(int16_t));
    } else {
      const size_t inter_off = off * static_cast<size_t>(file_mic_channels);
      for (int c = 0; c < num_mics; ++c) {
        for (int s = 0; s < frame_samples; ++s) {
          mic_f.ch[c][s] =
              mic_wav.samples[inter_off +
                              static_cast<size_t>(s) *
                                  static_cast<size_t>(file_mic_channels) +
                              static_cast<size_t>(c)];
        }
      }
    }

    // Capture the "before" frame BEFORE processing — ProcessCapture takes
    // mic_f by const ref today, but snapshotting here keeps the raw trace
    // decoupled from any future signature change.
    if (!args.out_raw.empty()) {
      raw.insert(raw.end(),
                 mic_f.ch[0].begin(),
                 mic_f.ch[0].begin() + frame_samples);
    }

    chain.ProcessRender(ref_f);
    chain.ProcessCapture(mic_f, out_f);
    processed.insert(processed.end(), out_f.ch[0].begin(),
                     out_f.ch[0].begin() + out_f.n_samples);
  }

  if (!ecnr::hal::WriteWavMono(args.out, processed, sample_rate_hz, &err)) {
    std::fprintf(stderr, "write out: %s\n", err.c_str());
    return 1;
  }
  if (!args.out_raw.empty()) {
    if (!ecnr::hal::WriteWavMono(args.out_raw, raw, sample_rate_hz, &err)) {
      std::fprintf(stderr, "write out-raw: %s\n", err.c_str());
      return 1;
    }
  }

  const auto& s = chain.Stats();
  // erle_db is sourced from APM stats (Task 6); under the Phase-0 stub the
  // optional is nullopt and we print N/A. Once the WebRTC backend lands,
  // the same code path emits the real value without a label change.
  std::printf("frames=%zu  audio=%.3fs  cpu=%.3fs  rtf=%.4f  mic_ch=%d  bf=%s",
              total_frames, s.audio_time_s, s.cpu_time_s, s.Rtf(),
              file_mic_channels,
              args.bypass_beamformer ? "bypass" : "dsb");
  if (s.echo_return_loss_enhancement_db.has_value()) {
    std::printf("  erle_db=%.2f", *s.echo_return_loss_enhancement_db);
  } else {
    std::printf("  erle_db=N/A");
  }
  std::printf("  dropped=%llu",
              static_cast<unsigned long long>(s.frames_dropped));
  if (args.ns_vad_blend_set) {
    std::printf("  ns_vad_blend=%.2f..%.2f", args.ns_vad_blend_low,
                args.ns_vad_blend_high);
    if (s.ns_vad_prob.has_value()) {
      std::printf("  last_vad=%.3f", *s.ns_vad_prob);
    }
    if (s.ns_current_blend.has_value()) {
      std::printf("  last_blend=%.3f", *s.ns_current_blend);
    }
  } else if (args.ns_dry_blend > 0.0f) {
    std::printf("  ns_dry_blend=%.3f", args.ns_dry_blend);
  }
  std::printf("\n");

  // A/B convenience: when both --out and --out-raw are set, print the
  // afplay pair so the listening test is one copy-paste away.
  if (!args.out_raw.empty()) {
    std::printf("A/B:  afplay %s   # before\n"
                "      afplay %s   # after\n",
                args.out_raw.c_str(), args.out.c_str());
  }
  return 0;
}

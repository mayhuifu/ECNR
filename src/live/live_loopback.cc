// ecnr_live — host-side end-to-end loopback test.
//
// Plays a known stimulus WAV out the default playback device while capturing
// from the default input device. Each captured 10 ms frame is paired with the
// matching played render frame, fed into AecChain (Phase 0 stub), and the
// processed output is appended to a result WAV. On exit, prints frame count,
// dropped-frame count, and the chain's RTF + last-frame ERLE.
//
// macOS: requires Microphone permission for the calling terminal app
// (System Settings → Privacy & Security → Microphone).
//
// This is the v1 host E2E harness. It exercises the full live audio path —
// capture → render-tap → AEC → output — without depending on the U300 HAL.
// Phase 0.5 swaps the stub for AEC3 + RNNoise; everything below is unchanged.

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/frame.h"
#include "core/ring_buffer.h"
#include "hal/file_io.h"
#include "pipeline/aec_chain.h"

namespace {

struct Args {
  std::string stimulus;       // WAV to play out the speakers (the "render" / far-end)
  std::string out = "live_out.wav";
  double duration_s = 0.0;    // 0 = play stimulus to completion
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
      "usage: %s --stimulus stim.wav [--out live_out.wav] [--duration SECONDS]\n",
      prog);
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--stimulus" && i + 1 < argc) a->stimulus = argv[++i];
    else if (flag == "--out" && i + 1 < argc) a->out = argv[++i];
    else if (flag == "--duration" && i + 1 < argc) a->duration_s = std::stod(argv[++i]);
    else if (flag == "-h" || flag == "--help") return false;
    else { std::fprintf(stderr, "unknown arg: %s\n", flag.c_str()); return false; }
  }
  return !a->stimulus.empty();
}

// Shared state between the miniaudio playback + capture callbacks and the
// processing thread. Single-producer-single-consumer per ring; the miniaudio
// callbacks write to capture_ring + render_ring, the processing thread reads.
struct LiveState {
  std::mutex mu;
  ecnr::RingBuffer capture_ring{static_cast<size_t>(ecnr::kSampleRateHz)};  // 1 s capacity
  ecnr::RingBuffer render_ring{static_cast<size_t>(ecnr::kSampleRateHz)};

  // Stimulus playback cursor.
  std::vector<int16_t> stimulus;
  size_t stim_pos = 0;
  bool stim_done = false;

  std::atomic<uint64_t> capture_dropped{0};
  std::atomic<uint64_t> render_dropped{0};

  std::atomic<bool> stop{false};
};

// Playback callback: pull next chunk of stimulus, write to output device,
// AND copy the same samples into render_ring as the AEC reference.
void PlaybackCallback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frame_count) {
  auto* st = static_cast<LiveState*>(dev->pUserData);
  auto* out = static_cast<int16_t*>(output);

  std::lock_guard<std::mutex> lk(st->mu);
  ma_uint32 written = 0;
  if (!st->stim_done) {
    const size_t remaining = st->stimulus.size() - st->stim_pos;
    const ma_uint32 take = static_cast<ma_uint32>(std::min<size_t>(remaining, frame_count));
    std::memcpy(out, st->stimulus.data() + st->stim_pos, take * sizeof(int16_t));
    st->stim_pos += take;
    written = take;
    if (st->stim_pos >= st->stimulus.size()) st->stim_done = true;
  }
  if (written < frame_count) {
    std::memset(out + written, 0, (frame_count - written) * sizeof(int16_t));
  }
  // The render ring records exactly what we sent to the device — the canonical
  // far-end reference for AEC.
  const size_t wrote_ref = st->render_ring.Write(out, frame_count);
  if (wrote_ref < frame_count) {
    st->render_dropped.fetch_add(frame_count - wrote_ref, std::memory_order_relaxed);
  }
}

void CaptureCallback(ma_device* dev, void* /*output*/, const void* input, ma_uint32 frame_count) {
  auto* st = static_cast<LiveState*>(dev->pUserData);
  const auto* in = static_cast<const int16_t*>(input);
  std::lock_guard<std::mutex> lk(st->mu);
  const size_t wrote = st->capture_ring.Write(in, frame_count);
  if (wrote < frame_count) {
    st->capture_dropped.fetch_add(frame_count - wrote, std::memory_order_relaxed);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::string err;
  ecnr::hal::WavData stim;
  if (!ecnr::hal::ReadWavMono(args.stimulus, &stim, &err)) {
    std::fprintf(stderr, "read stimulus: %s\n", err.c_str());
    return 1;
  }
  if (stim.sample_rate_hz != ecnr::kSampleRateHz) {
    std::fprintf(stderr, "stimulus must be %d Hz mono (got %d Hz)\n",
                 ecnr::kSampleRateHz, stim.sample_rate_hz);
    return 1;
  }

  LiveState st;
  st.stimulus = std::move(stim.samples);

  // Configure two devices: one playback, one capture. miniaudio's "duplex"
  // mode is also possible but separate devices are easier to reason about,
  // and our render-tap is an exact copy of what we send to playback.
  ma_device_config play_cfg = ma_device_config_init(ma_device_type_playback);
  play_cfg.playback.format   = ma_format_s16;
  play_cfg.playback.channels = 1;
  play_cfg.sampleRate        = ecnr::kSampleRateHz;
  play_cfg.dataCallback      = PlaybackCallback;
  play_cfg.pUserData         = &st;

  ma_device play_dev;
  if (ma_device_init(nullptr, &play_cfg, &play_dev) != MA_SUCCESS) {
    std::fprintf(stderr, "ma_device_init(playback) failed\n");
    return 1;
  }

  ma_device_config cap_cfg = ma_device_config_init(ma_device_type_capture);
  cap_cfg.capture.format   = ma_format_s16;
  cap_cfg.capture.channels = 1;
  cap_cfg.sampleRate       = ecnr::kSampleRateHz;
  cap_cfg.dataCallback     = CaptureCallback;
  cap_cfg.pUserData        = &st;

  ma_device cap_dev;
  if (ma_device_init(nullptr, &cap_cfg, &cap_dev) != MA_SUCCESS) {
    std::fprintf(stderr, "ma_device_init(capture) failed (microphone permission?)\n");
    ma_device_uninit(&play_dev);
    return 1;
  }

  ecnr::AecChain chain;
  if (!chain.Init(ecnr::kSampleRateHz)) {
    std::fprintf(stderr, "chain init failed\n");
    return 1;
  }

  if (ma_device_start(&cap_dev) != MA_SUCCESS) {
    std::fprintf(stderr, "ma_device_start(capture) failed\n");
    return 1;
  }
  if (ma_device_start(&play_dev) != MA_SUCCESS) {
    std::fprintf(stderr, "ma_device_start(playback) failed\n");
    ma_device_stop(&cap_dev);
    return 1;
  }

  std::printf("ecnr_live: playing %s, capturing from default mic. ctrl-c to abort.\n",
              args.stimulus.c_str());

  std::vector<int16_t> processed;
  processed.reserve(st.stimulus.size());

  ecnr::Frame mic_f, ref_f, out_f;
  const auto t_start = std::chrono::steady_clock::now();
  uint64_t frames_processed = 0;

  while (!st.stop.load()) {
    bool got_pair = false;
    {
      std::lock_guard<std::mutex> lk(st.mu);
      if (st.capture_ring.Size() >= ecnr::kFrameSamples &&
          st.render_ring.Size()  >= ecnr::kFrameSamples) {
        st.render_ring.Read(ref_f.samples.data(), ecnr::kFrameSamples);
        st.capture_ring.Read(mic_f.samples.data(), ecnr::kFrameSamples);
        got_pair = true;
      }
    }
    if (got_pair) {
      chain.ProcessRender(ref_f);
      chain.ProcessCapture(mic_f, out_f);
      processed.insert(processed.end(), out_f.samples.begin(), out_f.samples.end());
      ++frames_processed;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Termination conditions: stimulus done AND we've drained capture, or
    // explicit duration elapsed.
    if (args.duration_s > 0.0) {
      const auto elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - t_start).count();
      if (elapsed >= args.duration_s) break;
    } else if (st.stim_done) {
      // Allow a 200 ms drain for in-flight capture before stopping.
      static auto stim_done_at = std::chrono::steady_clock::time_point::min();
      if (stim_done_at == std::chrono::steady_clock::time_point::min()) {
        stim_done_at = std::chrono::steady_clock::now();
      }
      if (std::chrono::steady_clock::now() - stim_done_at >
          std::chrono::milliseconds(200)) {
        break;
      }
    }
  }

  ma_device_stop(&play_dev);
  ma_device_stop(&cap_dev);
  ma_device_uninit(&play_dev);
  ma_device_uninit(&cap_dev);

  if (!ecnr::hal::WriteWavMono(args.out, processed, ecnr::kSampleRateHz, &err)) {
    std::fprintf(stderr, "write out: %s\n", err.c_str());
    return 1;
  }

  const auto& s = chain.Stats();
  std::printf("frames=%llu  audio=%.3fs  rtf=%.4f  erle_last=%.2fdB  "
              "cap_dropped=%llu  ref_dropped=%llu\n",
              static_cast<unsigned long long>(frames_processed),
              s.audio_time_s, s.Rtf(), s.erle_db,
              static_cast<unsigned long long>(st.capture_dropped.load()),
              static_cast<unsigned long long>(st.render_dropped.load()));
  return 0;
}

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
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
  std::string out_raw;        // optional: raw captured-mic WAV (post-injection, pre-AEC)
  std::string inject_noise;   // optional: WAV mixed into capture stream before AEC
  double inject_gain_db = -12.0;  // gain applied to injected noise before mixing
  double duration_s = 0.0;    // 0 = play stimulus to completion
  // Record-only mode: no stimulus playback, no AEC; capture from default mic and
  // dump to the given WAV. Used to record a clean near-end voice signal for
  // gen_test_input.py (the deterministic A/B harness — see Step E.2 in README).
  std::string record_voice;
  bool out_explicit = false;  // true if --out was passed (vs. taken from default)
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
      "usage: %s --stimulus stim.wav [--out live_out.wav] [--out-raw raw.wav]\n"
      "       [--inject-noise noise.wav] [--inject-gain-db DB] [--duration SECONDS]\n"
      "       %s --record-voice out.wav [--duration SECONDS]\n"
      "\n"
      "  --stimulus FILE       (required for live mode) WAV played out the speakers (far-end / render)\n"
      "  --out FILE            processed AEC+NS output (default: live_out.wav)\n"
      "  --out-raw FILE        raw captured mic stream (post noise-injection, pre-AEC)\n"
      "                        — the 'before' track for A/B demos\n"
      "  --inject-noise FILE   WAV mixed into the capture stream before AEC sees it\n"
      "                        (must match stimulus rate, mono int16; loops if shorter)\n"
      "  --inject-gain-db DB   gain applied to injected noise (default: -12.0)\n"
      "  --duration SECONDS    cap session length (default: stimulus duration; or 15 s in --record-voice mode)\n"
      "  --record-voice FILE   record-only mode: capture from the default mic for --duration seconds\n"
      "                        (default 15 s) and write a 16 kHz mono WAV. No stimulus playback,\n"
      "                        no AEC chain. Incompatible with --out and --inject-noise.\n",
      prog, prog);
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--stimulus" && i + 1 < argc) a->stimulus = argv[++i];
    else if (flag == "--out" && i + 1 < argc) { a->out = argv[++i]; a->out_explicit = true; }
    else if (flag == "--out-raw" && i + 1 < argc) a->out_raw = argv[++i];
    else if (flag == "--inject-noise" && i + 1 < argc) a->inject_noise = argv[++i];
    else if (flag == "--inject-gain-db" && i + 1 < argc) a->inject_gain_db = std::stod(argv[++i]);
    else if (flag == "--duration" && i + 1 < argc) a->duration_s = std::stod(argv[++i]);
    else if (flag == "--record-voice" && i + 1 < argc) a->record_voice = argv[++i];
    else if (flag == "-h" || flag == "--help") return false;
    else { std::fprintf(stderr, "unknown arg: %s\n", flag.c_str()); return false; }
  }
  // In record-voice mode, --stimulus is not required. In live mode, it is.
  if (!a->record_voice.empty()) return true;
  return !a->stimulus.empty();
}

// Shared state between the miniaudio playback + capture callbacks and the
// processing thread. Single-producer-single-consumer per ring; the miniaudio
// callbacks write to capture_ring + render_ring, the processing thread reads.
struct LiveState {
  std::mutex mu;
  // 1 second of capacity at the maximum supported rate. Sized to the upper
  // bound (48 kHz) so the same struct works for either tier; over-provisioning
  // ~96 KB per ring is harmless on host.
  ecnr::RingBuffer capture_ring{48000};
  ecnr::RingBuffer render_ring{48000};

  // Stimulus playback cursor. `stim_done` is atomic because the playback
  // callback writes it on the audio thread and the main thread reads it
  // OUTSIDE the mutex on each loop iteration. Without atomicity the read
  // is undefined behavior under the C++ memory model and empirically does
  // not become visible on macOS Apple Silicon — the program then runs
  // forever and only exits on ctrl-C.
  std::vector<int16_t> stimulus;
  size_t stim_pos = 0;
  std::atomic<bool> stim_done{false};

  std::atomic<uint64_t> capture_dropped{0};
  std::atomic<uint64_t> render_dropped{0};

  // Optional noise-injection state. The capture callback is the only thread
  // that reads `inject_samples` and the only writer of `inject_pos`, so no
  // locking is needed for these — they're effectively callback-local. The
  // gain is precomputed at startup from --inject-gain-db.
  std::vector<int16_t> inject_samples;  // empty => injection disabled
  size_t inject_pos = 0;
  float inject_linear_gain = 1.0f;

  std::atomic<bool> stop{false};
};

// Set by the SIGINT handler so ctrl-C drains the loop and writes the WAV
// instead of killing the process before output.
std::atomic<bool>* g_stop_flag = nullptr;
void HandleSigint(int /*sig*/) {
  if (g_stop_flag) g_stop_flag->store(true, std::memory_order_release);
}

// Playback callback: pull next chunk of stimulus, write to output device,
// AND copy the same samples into render_ring as the AEC reference.
void PlaybackCallback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frame_count) {
  auto* st = static_cast<LiveState*>(dev->pUserData);
  auto* out = static_cast<int16_t*>(output);

  std::lock_guard<std::mutex> lk(st->mu);
  ma_uint32 written = 0;
  if (!st->stim_done.load(std::memory_order_acquire)) {
    const size_t remaining = st->stimulus.size() - st->stim_pos;
    const ma_uint32 take = static_cast<ma_uint32>(std::min<size_t>(remaining, frame_count));
    std::memcpy(out, st->stimulus.data() + st->stim_pos, take * sizeof(int16_t));
    st->stim_pos += take;
    written = take;
    if (st->stim_pos >= st->stimulus.size()) {
      st->stim_done.store(true, std::memory_order_release);
    }
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

  // Fast path with no noise injection: copy mic samples straight into the ring.
  if (st->inject_samples.empty()) {
    std::lock_guard<std::mutex> lk(st->mu);
    const size_t wrote = st->capture_ring.Write(in, frame_count);
    if (wrote < frame_count) {
      st->capture_dropped.fetch_add(frame_count - wrote, std::memory_order_relaxed);
    }
    return;
  }

  // Software-mix path: pull `frame_count` samples from the looped noise buffer,
  // apply linear gain, add to the mic samples with int16 saturation, then
  // forward the mixed stream to the ring. The chain (and --out-raw) sees the
  // mixed signal, not the bare mic. This is "Option A" — injection happens
  // BEFORE AEC, and crucially the render/render-tap still carries only the
  // stimulus, so AEC3 treats the injected noise as near-end noise (not echo)
  // and leaves it for RNNoise to suppress.
  std::vector<int16_t> mixed(frame_count);
  const size_t noise_n = st->inject_samples.size();
  for (ma_uint32 i = 0; i < frame_count; ++i) {
    const float noise = static_cast<float>(st->inject_samples[st->inject_pos]) *
                        st->inject_linear_gain;
    st->inject_pos = (st->inject_pos + 1) % noise_n;
    float sum = static_cast<float>(in[i]) + noise;
    if (sum > 32767.0f) sum = 32767.0f;
    if (sum < -32768.0f) sum = -32768.0f;
    mixed[i] = static_cast<int16_t>(sum);
  }

  std::lock_guard<std::mutex> lk(st->mu);
  const size_t wrote = st->capture_ring.Write(mixed.data(), frame_count);
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

  // --- Record-voice mode (no stimulus, no AEC, no playback). ---
  // This branches early because the rest of main() assumes a stimulus and an
  // AEC chain. Record mode is a much simpler capture-only loop that writes the
  // raw mic stream to a 16 kHz mono WAV — used for the deterministic A/B
  // harness (Step E.2 in README).
  if (!args.record_voice.empty()) {
    if (args.out_explicit) {
      std::fprintf(stderr,
                   "--record-voice is incompatible with --out (no AEC chain to produce output)\n");
      return 2;
    }
    if (!args.inject_noise.empty()) {
      std::fprintf(stderr,
                   "--record-voice is incompatible with --inject-noise (no AEC chain to inject into)\n");
      return 2;
    }

    constexpr int kRecordSampleRate = 16000;
    const double duration_s = args.duration_s > 0.0 ? args.duration_s : 15.0;

    LiveState st;
    g_stop_flag = &st.stop;
    std::signal(SIGINT, HandleSigint);
    std::signal(SIGTERM, HandleSigint);

    ma_device_config cap_cfg = ma_device_config_init(ma_device_type_capture);
    cap_cfg.capture.format   = ma_format_s16;
    cap_cfg.capture.channels = 1;
    cap_cfg.sampleRate       = static_cast<ma_uint32>(kRecordSampleRate);
    cap_cfg.dataCallback     = CaptureCallback;
    cap_cfg.pUserData        = &st;

    ma_device cap_dev;
    if (ma_device_init(nullptr, &cap_cfg, &cap_dev) != MA_SUCCESS) {
      std::fprintf(stderr, "ma_device_init(capture) failed (microphone permission?)\n");
      return 1;
    }
    if (ma_device_start(&cap_dev) != MA_SUCCESS) {
      std::fprintf(stderr, "ma_device_start(capture) failed\n");
      ma_device_uninit(&cap_dev);
      return 1;
    }

    std::printf("ecnr_live: record-voice mode — capturing %.1f s from default mic to %s. "
                "speak now (ctrl-c to stop early).\n",
                duration_s, args.record_voice.c_str());

    // Drain the capture ring into an accumulator, no AEC chain. Same loop
    // shape as the live path so SIGINT and --duration semantics match exactly.
    std::vector<int16_t> recorded;
    const size_t expected = static_cast<size_t>(duration_s * kRecordSampleRate);
    recorded.reserve(expected);

    const auto t_start = std::chrono::steady_clock::now();
    std::vector<int16_t> scratch(1024);
    while (!st.stop.load()) {
      size_t got = 0;
      {
        std::lock_guard<std::mutex> lk(st.mu);
        got = st.capture_ring.Read(scratch.data(), scratch.size());
      }
      if (got > 0) {
        recorded.insert(recorded.end(), scratch.begin(), scratch.begin() + got);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }

      const auto now = std::chrono::steady_clock::now();
      const double elapsed = std::chrono::duration<double>(now - t_start).count();
      if (elapsed >= duration_s) break;
    }

    ma_device_stop(&cap_dev);
    ma_device_uninit(&cap_dev);

    // Final drain of anything still queued in the ring after the device stops.
    {
      std::lock_guard<std::mutex> lk(st.mu);
      while (st.capture_ring.Size() > 0) {
        const size_t got = st.capture_ring.Read(scratch.data(), scratch.size());
        if (got == 0) break;
        recorded.insert(recorded.end(), scratch.begin(), scratch.begin() + got);
      }
    }

    if (!ecnr::hal::WriteWavMono(args.record_voice, recorded, kRecordSampleRate, &err)) {
      std::fprintf(stderr, "write record-voice: %s\n", err.c_str());
      return 1;
    }

    const double recorded_s =
        static_cast<double>(recorded.size()) / kRecordSampleRate;
    std::printf("record-voice mode: wrote %zu samples (%.2f s) to %s; cap_dropped=%llu\n",
                recorded.size(), recorded_s, args.record_voice.c_str(),
                static_cast<unsigned long long>(st.capture_dropped.load()));
    return 0;
  }

  ecnr::hal::WavData stim;
  if (!ecnr::hal::ReadWavMono(args.stimulus, &stim, &err)) {
    std::fprintf(stderr, "read stimulus: %s\n", err.c_str());
    return 1;
  }
  if (!ecnr::IsSupportedSampleRate(stim.sample_rate_hz)) {
    std::fprintf(stderr,
                 "stimulus must be 16000 or 48000 Hz mono (got %d Hz)\n",
                 stim.sample_rate_hz);
    return 1;
  }
  const int sample_rate_hz = stim.sample_rate_hz;
  const int frame_samples = ecnr::FrameSamplesFor(sample_rate_hz);

  LiveState st;
  st.stimulus = std::move(stim.samples);

  // Optional noise injection: load + validate before opening the audio devices
  // so a misconfiguration fails fast instead of mid-session.
  if (!args.inject_noise.empty()) {
    ecnr::hal::WavData noise;
    if (!ecnr::hal::ReadWavMono(args.inject_noise, &noise, &err)) {
      std::fprintf(stderr, "read inject-noise: %s\n", err.c_str());
      return 1;
    }
    if (!ecnr::IsSupportedSampleRate(noise.sample_rate_hz) ||
        noise.sample_rate_hz != sample_rate_hz) {
      std::fprintf(stderr,
                   "inject-noise sample rate (%d Hz) must match stimulus rate (%d Hz)\n",
                   noise.sample_rate_hz, sample_rate_hz);
      return 1;
    }
    if (noise.samples.empty()) {
      std::fprintf(stderr, "inject-noise WAV is empty\n");
      return 1;
    }
    st.inject_samples = std::move(noise.samples);
    st.inject_linear_gain =
        static_cast<float>(std::pow(10.0, args.inject_gain_db / 20.0));
  }

  // Install a ctrl-C handler that flips st.stop. Without this, ctrl-C
  // terminates the process before WriteWavMono runs and no output WAV is
  // produced — exactly what bit the first user run.
  g_stop_flag = &st.stop;
  std::signal(SIGINT, HandleSigint);
  std::signal(SIGTERM, HandleSigint);

  // Defensive hard timeout: stimulus duration + 3 s drain. Even if stim_done
  // detection or the stop flag misbehave, the program is guaranteed to exit
  // on its own within this window. The user can still set --duration N
  // explicitly to override.
  const double stimulus_seconds =
      static_cast<double>(st.stimulus.size()) / sample_rate_hz;
  const double hard_timeout_s = stimulus_seconds + 3.0;

  // Configure two devices: one playback, one capture. miniaudio's "duplex"
  // mode is also possible but separate devices are easier to reason about,
  // and our render-tap is an exact copy of what we send to playback.
  ma_device_config play_cfg = ma_device_config_init(ma_device_type_playback);
  play_cfg.playback.format   = ma_format_s16;
  play_cfg.playback.channels = 1;
  play_cfg.sampleRate        = static_cast<ma_uint32>(sample_rate_hz);
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
  cap_cfg.sampleRate       = static_cast<ma_uint32>(sample_rate_hz);
  cap_cfg.dataCallback     = CaptureCallback;
  cap_cfg.pUserData        = &st;

  ma_device cap_dev;
  if (ma_device_init(nullptr, &cap_cfg, &cap_dev) != MA_SUCCESS) {
    std::fprintf(stderr, "ma_device_init(capture) failed (microphone permission?)\n");
    ma_device_uninit(&play_dev);
    return 1;
  }

  // ADR-0004: production num_mics ∈ [2, 8]. The host live harness has only
  // a single physical mic, so we initialize at num_mics = 2 and duplicate
  // the mono capture into ch[0] and ch[1]. The Beamformer stub picks ch[0],
  // so the duplicate channel is a no-op but keeps the chain interface
  // honest with the production contract.
  // TODO(ADR-0010): mono-duplicated 2-channel input is degenerate for any real
  // beamformer (zero inter-mic delay; perfectly correlated channels — singular
  // covariance under MVDR). Once the real beamformer lands, this harness should
  // either accept multi-channel WAV input or add a --bypass-beamformer flag.
  ecnr::AecChain chain;
  if (!chain.Init(sample_rate_hz, 2)) {
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
  // `raw` mirrors `processed` for the "before" track. It's the post-injection
  // mic stream — exactly what we hand to AEC — which is the right reference
  // for an A/B listening test ("here's what the chain saw; here's what it
  // produced"). When --inject-noise is unset, raw == bare mic capture.
  std::vector<int16_t> raw;
  if (!args.out_raw.empty()) {
    raw.reserve(st.stimulus.size());
  }

  ecnr::Frame mic_f, ref_f, out_f;
  const auto t_start = std::chrono::steady_clock::now();
  uint64_t frames_processed = 0;

  while (!st.stop.load()) {
    bool got_pair = false;
    {
      std::lock_guard<std::mutex> lk(st.mu);
      if (static_cast<int>(st.capture_ring.Size()) >= frame_samples &&
          static_cast<int>(st.render_ring.Size())  >= frame_samples) {
        // Render is mono.
        ref_f.n_samples = frame_samples;
        ref_f.n_channels = 1;
        st.render_ring.Read(ref_f.ch[0].data(),
                            static_cast<size_t>(frame_samples));
        // Mic is 2-channel; read mono into ch[0], then duplicate to ch[1].
        mic_f.n_samples = frame_samples;
        mic_f.n_channels = 2;
        st.capture_ring.Read(mic_f.ch[0].data(),
                             static_cast<size_t>(frame_samples));
        std::memcpy(mic_f.ch[1].data(), mic_f.ch[0].data(),
                    frame_samples * sizeof(int16_t));
        got_pair = true;
      }
    }
    if (got_pair) {
      // Snapshot the mic frame BEFORE the chain mutates anything (currently
      // ProcessCapture takes mic_f by const ref, but capturing here keeps the
      // raw trace decoupled from any future signature change).
      if (!args.out_raw.empty()) {
        raw.insert(raw.end(), mic_f.ch[0].begin(),
                   mic_f.ch[0].begin() + mic_f.n_samples);
      }
      chain.ProcessRender(ref_f);
      chain.ProcessCapture(mic_f, out_f);
      processed.insert(processed.end(), out_f.ch[0].begin(),
                       out_f.ch[0].begin() + out_f.n_samples);
      ++frames_processed;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Termination conditions, in priority order:
    //   1. SIGINT/SIGTERM (st.stop set by handler) — drain & write WAV.
    //   2. Explicit --duration N elapsed.
    //   3. Stimulus exhausted + 200 ms drain for in-flight capture.
    //   4. Hard timeout (stimulus_duration + 3 s) as a defensive backstop.
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - t_start).count();

    if (st.stop.load(std::memory_order_acquire)) break;

    if (args.duration_s > 0.0 && elapsed >= args.duration_s) break;

    if (st.stim_done.load(std::memory_order_acquire)) {
      // Allow a 200 ms drain for in-flight capture before stopping.
      static auto stim_done_at = std::chrono::steady_clock::time_point::min();
      if (stim_done_at == std::chrono::steady_clock::time_point::min()) {
        stim_done_at = now;
      }
      if (now - stim_done_at > std::chrono::milliseconds(200)) {
        break;
      }
    }

    if (elapsed >= hard_timeout_s) {
      std::fprintf(stderr,
                   "ecnr_live: hard timeout (%.1fs) reached before stim_done; "
                   "draining and writing partial output\n",
                   hard_timeout_s);
      break;
    }
  }

  ma_device_stop(&play_dev);
  ma_device_stop(&cap_dev);
  ma_device_uninit(&play_dev);
  ma_device_uninit(&cap_dev);

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
  std::printf("frames=%llu  audio=%.3fs  rtf=%.4f",
              static_cast<unsigned long long>(frames_processed),
              s.audio_time_s, s.Rtf());
  if (s.echo_return_loss_enhancement_db.has_value()) {
    std::printf("  erle_db=%.2f", *s.echo_return_loss_enhancement_db);
  } else {
    std::printf("  erle_db=N/A");
  }
  std::printf("  cap_dropped=%llu  ref_dropped=%llu  chain_dropped=%llu",
              static_cast<unsigned long long>(st.capture_dropped.load()),
              static_cast<unsigned long long>(st.render_dropped.load()),
              static_cast<unsigned long long>(s.frames_dropped));
  if (!args.inject_noise.empty()) {
    std::printf("  inject_noise=%s (%.1f dB)",
                args.inject_noise.c_str(), args.inject_gain_db);
  }
  std::printf("\n");
  return 0;
}

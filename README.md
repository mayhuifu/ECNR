# ECNR

Automotive in-cabin AEC + NR audio stack for the U300 system. Hybrid architecture: WebRTC AEC3 linear backbone + neural post-processing for residual echo and noise.

See [PROJECT.md](PROJECT.md) for the architecture, roadmap, and dependency list. See [docs/](docs) for the underlying research.

## Status

Phase 0 (bootstrap) **done**: scaffold + stub AEC/NS backends + dev-host smoke test green. Phase 0.6 (host live E2E loopback via miniaudio) **done**. Phase 0.5 (real WebRTC AEC3 + RNNoise wiring) is next. Not yet cross-compiled for A55. Not yet vehicle-validated.

Vendored open-source dependencies are not in git — fetch on demand:

```sh
scripts/fetch-vendor.sh required   # ~5 MB build deps only
scripts/fetch-vendor.sh            # ~450 MB inc. research code + pretrained models
```

## Prerequisites (dev host: Linux or macOS)

- C++17 compiler (clang or gcc)
- CMake ≥ 3.20
- `pkg-config` and `libsndfile`
- Python 3 + `numpy` (for generating synthetic test WAVs)

macOS:

```sh
brew install cmake pkg-config libsndfile
```

Linux (Debian/Ubuntu):

```sh
apt install cmake pkg-config libsndfile1-dev build-essential python3-numpy
```

`googletest` is fetched at configure time — no separate install needed. `miniaudio` is vendored at `third_party/miniaudio/`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Three binaries land in `build/`:

| Binary | Purpose |
|---|---|
| `build/aec_chain_test` | gtest unit/integration tests for `AecChain` |
| `build/ecnr_bench` | Offline replay harness — feeds two WAVs (mic + ref) through the chain, writes processed output, reports RTF + ERLE |
| `build/ecnr_live` | Live loopback — plays a stimulus through your speakers, captures from your mic, runs the chain in real time |

---

## Testing & verification — step by step

This is the canonical procedure to confirm every piece of the stack works on your machine. Run it after a fresh checkout, after a build-system change, or before declaring a phase done. Each step has a **command**, an **expected result**, and a one-line statement of **what it proves**.

### Step A — Clean build

```sh
rm -rf build && \
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
cmake --build build -j
```

**Expected (last few lines):**

```
[100%] Built target aec_chain_test
[100%] Linking CXX executable ecnr_live
[100%] Built target ecnr_live
```

No errors. One harmless warning (`unused-function 'FrameEnergyDb'`) is fine if you see it.

**Proves:** Toolchain, CMake config, libsndfile, googletest fetch, miniaudio header, and CoreAudio framework linkage are all healthy.

### Step B — Unit & integration tests

```sh
ctest --test-dir build --output-on-failure
```

**Expected:**

```
1/3 Test #1: AecChain.InitRejectsWrongSampleRate ...   Passed
2/3 Test #2: AecChain.AttenuatesCorrelatedEcho .....   Passed
3/3 Test #3: AecChain.RtfIsMeasured ................   Passed

100% tests passed, 0 tests failed out of 3
```

**Proves:** `AecChain` rejects unsupported sample rates, the (Phase 0 stub) AEC actually attenuates correlated echo (cumulative ERLE > 5 dB on 200 noise frames), and the chain measures CPU/audio time correctly (RTF < 1).

> Phase 0.5 will tighten the ERLE assertion to **> 15 dB** once WebRTC AEC3 + RNNoise replace the stubs. If you ever see this test fail with "ERLE > 15 dB", it means the wiring is correct and you've crossed into Phase 0.5 territory.

### Step C — Generate synthetic test audio

```sh
python3 reference/gen_synth.py --duration 10 --out-dir reference/synth/
```

**Expected:**

```
wrote reference/synth/{ref,mic,near_clean}.wav  (10.0s @ 16000 Hz)
```

This produces three 10-second 16 kHz mono WAVs:

| File | Contents |
|---|---|
| `ref.wav` | White noise — the "far-end" stimulus, what the speaker would play |
| `mic.wav` | `ref.wav` convolved with a 30 ms synthetic IR + a low-amplitude near-end tone |
| `near_clean.wav` | The near-end tone alone (the "oracle" — what AEC should ideally recover) |

**Proves:** Python tooling and synthetic-corpus generation work. These files are git-ignored — regenerate on demand.

### Step D — Offline replay (deterministic)

```sh
./build/ecnr_bench \
  --mic reference/synth/mic.wav \
  --ref reference/synth/ref.wav \
  --out /tmp/ecnr_offline_out.wav
```

**Expected (numbers will vary slightly):**

```
frames=1000  audio=10.000s  cpu=0.001s  rtf=0.0001  erle_db=0.00 (stub: 0)
```

**How to read it:**
- `frames=1000` and `audio=10.000s` → the harness consumed all 10 s of the input pair correctly.
- `rtf` ≪ 1.0 → the chain is faster than realtime (under stubs it's trivially fast; under Phase 0.5 expect roughly 0.05–0.20 on this M-class machine).
- `erle_db` comes from `ChainStats::echo_return_loss_enhancement_db`, an `std::optional<double>` that mirrors `webrtc::AudioProcessingStats::echo_return_loss_enhancement` (per ADR-0006). Under the Phase 0 stub the optional is `nullopt` and the printout falls back to `0.00`; the real number arrives once the WebRTC APM backend is wired in Task 6. The unit test (Step B) checks **cumulative** energy attenuation directly, which is the right signal under stubs.

```sh
ls -la /tmp/ecnr_offline_out.wav  # should be ~320 KB (10 s × 16 kHz × 2 bytes)
```

**Proves:** WAV I/O, frame-by-frame chain processing, RTF accounting, and output writing all work end-to-end on deterministic input.

### Step E — Live host loopback (real speaker + real mic)

> macOS will prompt for **Microphone permission** the first time. Approve for your terminal app (System Settings → Privacy & Security → Microphone). On second-and-later runs no prompt.
>
> Wear headphones if you don't want the room to hear the stimulus, OR run with the volume low. The stimulus is 10 s of white noise.

```sh
./build/ecnr_live \
  --stimulus reference/synth/ref.wav \
  --out /tmp/ecnr_live_out.wav
```

**Expected (numbers depend on your room):**

```
ecnr_live: playing reference/synth/ref.wav, capturing from default mic. ctrl-c to abort.
frames=1000  audio=10.000s  rtf=...  erle_db=0.00 (stub: 0)  cap_dropped=0  ref_dropped=0
```

**How to read it:**
- `cap_dropped=0` and `ref_dropped=0` → no buffer overruns; capture and render kept up. Non-zero numbers indicate either a thread starvation issue or a too-small ring-buffer (currently 1 s / 16000 samples per ring).
- `frames=1000` (matching the 10 s stimulus / 10 ms frames) → live loop ran cleanly to completion.
- `rtf` should be similar to the offline run (Step D). Big swings indicate scheduling or device-side jitter.
- `erle_db` falls back to `0.00` under the Phase 0 stub (the optional `echo_return_loss_enhancement_db` is `nullopt`). Real values land in Task 6 with the WebRTC backend.

**Listening test:**

```sh
afplay /tmp/ecnr_live_out.wav   # macOS
```

You should hear roughly your room ambient noise without (or with reduced) the stimulus echo. Under the stub it'll be a partial cancellation; under Phase 0.5 the stimulus echo should be substantially gone.

**Proves:** CoreAudio backend works, miniaudio capture + playback callbacks plumb correctly into the ring buffers, the AEC chain processes a live stream without dropping frames, and the captured echo can be removed (to whatever quality the current backend provides).

### Step F — Re-fetch vendor (optional, slow)

```sh
scripts/fetch-vendor.sh required   # production baselines only (~5 MB)
# or:
scripts/fetch-vendor.sh            # everything (~450 MB)
```

**Expected:** lines like `  ok   webrtc-audio-processing @ d0569cfa…` for each repo. Re-running is idempotent — already-present repos at the pinned SHA print `ok` and do nothing.

**Proves:** The manifest pinning + clone logic works. Required for Phase 0.5 onwards (when the build actually links against vendored code). Not required for Phase 0/0.6.

---

## Common failures & fixes

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmake: command not found` | Dev tools not installed | `brew install cmake pkg-config libsndfile` |
| `Could NOT find PkgConfig` | `pkg-config` missing | Same as above |
| `Checking for module 'sndfile' — not found` | libsndfile not installed *or* pkg-config can't find it | `brew install libsndfile` then re-run cmake configure |
| `ma_device_init(capture) failed (microphone permission?)` | Mac mic permission not granted | System Settings → Privacy & Security → Microphone → enable for your terminal app, restart terminal |
| Live loopback prints `cap_dropped=NNN` (non-zero) | Processing thread starved, or device buffer larger than ring | Either: build `Release` not `RelWithDebInfo`; or open an issue and share the number for tuning |
| `stimulus must be 16000 Hz mono` | Stimulus WAV at wrong rate | Regenerate with `gen_synth.py` or resample with `sox in.wav -r 16000 -c 1 out.wav` |
| Test 2 (`AttenuatesCorrelatedEcho`) fails after a backend change | You're in Phase 0.5+ — the stub threshold of 5 dB is too lenient. Tighten the assertion to match what the real AEC delivers. | Edit `src/tests/aec_chain_test.cc` |

---

## Layout

| Path | Purpose |
|---|---|
| `docs/` | Background research (CN + EN deep-research reports + 5G VoNR PDF) |
| `vendor/` | Pinned upstream open-source deps (gitignored; see `MANIFEST.tsv`) |
| `src/core/` | Frame, ring buffer, resampler |
| `src/pipeline/` | `AecChain` — AEC3 + RNNoise wire-up (stubbed in Phase 0) |
| `src/hal/` | Mic/render abstraction (file-backed via libsndfile for v1) |
| `src/bench/` | `ecnr_bench` — offline file replay harness |
| `src/live/` | `ecnr_live` — live host loopback (miniaudio mic + speaker) |
| `src/tests/` | gtest |
| `models/` | Neural model artifacts (Phase 3+) |
| `reference/` | Test audio + synthesizer (`gen_synth.py`) |
| `scripts/` | Vendor fetcher (`fetch-vendor.sh`) |
| `third_party/` | Vendored single-header deps (miniaudio) and FetchContent doc |

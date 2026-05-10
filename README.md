# ECNR

Automotive in-cabin AEC + NR audio stack for the U300 system. Hybrid architecture: WebRTC AEC3 linear backbone + neural post-processing for residual echo and noise.

See [PROJECT.md](PROJECT.md) for the architecture, roadmap, and dependency list. See [docs/](docs) for the underlying research.

## Status

Phase 0 (bootstrap) **done**: scaffold + dev-host smoke test green. Phase 0.6 (host live E2E loopback via miniaudio) **done**. Phase 0.5 in progress: real WebRTC AEC3 wired (Task 6); RNNoise NS swap is next (Task 7). NS backend remains a stub. Not yet cross-compiled for A55. Not yet vehicle-validated.

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
1/8 Test #1: AecChain.InitRejectsWrongSampleRate ........................   Passed
2/8 Test #2: AecChain.InitRejectsWrongMicCount ..........................   Passed
3/8 Test #3: AecChain.AcceptsBoth16kAnd48k ..............................   Passed
4/8 Test #4: AecChain.AttenuatesCorrelatedEcho ..........................   Passed
5/8 Test #5: AecChain.BeamformerStubCollapsesToCh0 ......................   Passed
6/8 Test #6: AecChain.SetStreamDelayMsAcceptsAndClamps ..................   Passed
7/8 Test #7: AecChain.EchoReturnLossEnhancementOptionalUnsetUnderStub ...   Passed
8/8 Test #8: AecChain.RtfIsMeasured .....................................   Passed

100% tests passed, 0 tests failed out of 8
```

**Proves:** `AecChain` rejects unsupported sample rates and out-of-range mic counts (per ADR-0003 + ADR-0004), accepts both 16 kHz and 48 kHz, the stub Beamformer collapses N-channel input to ch[0], the (Phase 0 stub) AEC actually attenuates correlated echo (cumulative ERLE > 5 dB on 200 noise frames), and the chain measures CPU/audio time correctly (RTF < 1).

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

**Expected (numbers vary by machine; ERLE typically 12–20 dB on this synthetic stimulus):**

```
frames=1000  audio=10.000s  cpu=0.57s  rtf=0.057  erle_db=16.68  dropped=0
```

**How to read it:**
- `frames=1000` and `audio=10.000s` → the harness consumed all 10 s of the input pair correctly.
- `rtf` ≪ 1.0 → the chain is faster than realtime; on an M-class laptop expect roughly 0.04–0.10 with AEC3 + RNNoise wired (16k tier adds Speex 16↔48 kHz resampling around RNNoise).
- `erle_db` is `webrtc::AudioProcessingStats::echo_return_loss_enhancement` surfaced through `ChainStats::echo_return_loss_enhancement_db` (an `std::optional<double>`, per ADR-0006). With real AEC3 wired (Task 6) it populates after a brief warm-up; the bench prints `N/A` only if the optional is still unset (e.g., the stimulus was so short AEC3 didn't have time to converge). On the synthetic mic/ref pair above, expect a value in the 12–20 dB range.
- `dropped=0` — non-zero means a HAL/harness bug fed the chain a frame with wrong rate/channels/samples; should always be zero in healthy runs.

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

**Expected (numbers depend on your room; ERLE values vary widely):**

```
ecnr_live: playing reference/synth/ref.wav, capturing from default mic. ctrl-c to abort.
frames=1000  audio=10.000s  rtf=0.02  erle_db=2.40  cap_dropped=0  ref_dropped=0  chain_dropped=0
```

**How to read it:**
- `cap_dropped=0` and `ref_dropped=0` → no buffer overruns; capture and render kept up. Non-zero numbers indicate either a thread starvation issue or a too-small ring-buffer (currently 1 s / 16000 samples per ring).
- `chain_dropped=0` — non-zero means a HAL/harness bug fed the chain a frame with wrong rate/channels/samples; should always be zero in healthy runs.
- `frames=1000` (matching the 10 s stimulus / 10 ms frames) → live loop ran cleanly to completion.
- `rtf` should be similar to the offline run (Step D). Big swings indicate scheduling or device-side jitter.
- `erle_db` is the real `webrtc::AudioProcessingStats::echo_return_loss_enhancement` (surfaced as `ChainStats::echo_return_loss_enhancement_db`, an `std::optional<double>`). In a quiet room with no real speaker→mic loop (e.g. headphones in, low volume) the value is small — close to 0 dB — because AEC3 has very little echo to cancel; in a loud room with the speaker driving the mic, expect higher numbers. The bench prints `N/A` only if the optional is still unset (extremely short stimulus / no convergence).

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
| `stimulus must be 16000 or 48000 Hz mono` | Stimulus WAV at wrong rate | Regenerate with `gen_synth.py` or resample with `sox in.wav -r 16000 -c 1 out.wav` (or `-r 48000`) |
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

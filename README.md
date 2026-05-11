# ECNR

Automotive in-cabin AEC + NR audio stack for the U300 system. Hybrid architecture: WebRTC AEC3 linear backbone + neural post-processing for residual echo and noise.

See [PROJECT.md](PROJECT.md) for the architecture, roadmap, and dependency list. See [docs/](docs) for the underlying research.

## Status

Phase 0 (bootstrap) **done**: scaffold + dev-host smoke test green. Phase 0.6 (host live E2E loopback via miniaudio) **done**. Phase 0.5 (real WebRTC AEC3 + RNNoise + multi-rate + multi-mic `Frame` + tightened ERLE thresholds) **done**: WebRTC AEC3 wired (Task 6), RNNoise NS wired with Speex 16↔48 kHz resampling (Task 7), and integration tests tightened to require ERLE > 15 dB on the synthetic stimulus (Task 8) — measured ~64 dB at 16 k / ~62 dB at 48 k, RTF ~0.06 on macOS Apple Silicon. The host live loopback (`ecnr_live`) builds and runs but still requires the user's interactive listening test on real hardware — see Step E below. Not yet cross-compiled for A55. Not yet vehicle-validated.

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

Four binaries land in `build/`:

| Binary | Purpose |
|---|---|
| `build/aec_chain_test` | gtest integration tests for the full `AecChain` (12 cases — sample-rate/mic-count gates, ERLE > 15 dB on correlated echo at 16 k and 48 k, NS active at both rates, AEC3 stats convergence, RTF, etc.) |
| `build/beamformer_test` | gtest unit tests for the `Beamformer` collapse stage (6 cases — input validation, ch[0]-only collapse, independence from non-ch[0] channels, 16 k + 48 k coverage) |
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

**Expected (last few lines; ordering of the parallel link steps may vary):**

```
[ 93%] Linking CXX executable ecnr_bench
[ 93%] Built target ecnr_bench
[ 95%] Linking CXX executable beamformer_test
[ 97%] Linking CXX executable aec_chain_test
[ 97%] Built target beamformer_test
[ 97%] Built target aec_chain_test
[100%] Linking CXX executable ecnr_live
[100%] Built target ecnr_live
```

No errors. The first configure + build is slow (a few minutes) because it builds the vendored `webrtc-audio-processing` via meson at configure time; subsequent incremental rebuilds are fast.

**Proves:** Toolchain, CMake config, libsndfile, speexdsp + abseil (via pkg-config), googletest fetch, the meson-built WebRTC AEC3 + RNNoise vendor backends, miniaudio header, and CoreAudio framework linkage are all healthy.

### Step B — Unit & integration tests

```sh
ctest --test-dir build --output-on-failure
```

**Expected (per-test timings vary by machine; the list and pass/fail outcome should not):**

```
 1/18 Test  #1: AecChain.InitRejectsWrongSampleRate ..................   Passed
 2/18 Test  #2: AecChain.InitRejectsWrongMicCount ....................   Passed
 3/18 Test  #3: AecChain.AcceptsBoth16kAnd48k ........................   Passed
 4/18 Test  #4: AecChain.AttenuatesCorrelatedEcho ....................   Passed
 5/18 Test  #5: AecChain.BeamformerCollapsesToMono ...................   Passed
 6/18 Test  #6: AecChain.SetStreamDelayMsAcceptsAndClamps ............   Passed
 7/18 Test  #7: AecChain.ChainStatsPopulatedAfterAec3Convergence .....   Passed
 8/18 Test  #8: AecChain.RejectsMisshapedFrame .......................   Passed
 9/18 Test  #9: AecChain.RtfIsMeasured ...............................   Passed
10/18 Test #10: AecChain.NsActiveAtBothRates .........................   Passed
11/18 Test #11: AecChain.AttenuatesCorrelatedEchoAt48k ...............   Passed
12/18 Test #12: AecChain.ProcessesAt8Mics ............................   Passed
13/18 Test #13: Beamformer.InitRejectsUnsupportedSampleRate ..........   Passed
14/18 Test #14: Beamformer.InitRejectsUnsupportedMicCount ............   Passed
15/18 Test #15: Beamformer.ProcessCollapsesToCh0Verbatim_16k_4Mics ...   Passed
16/18 Test #16: Beamformer.ProcessIndependentOfNonCh0Channels ........   Passed
17/18 Test #17: Beamformer.ProcessAt48kAlsoCollapses .................   Passed
18/18 Test #18: Beamformer.ProcessRejectsMisshapedInput ..............   Passed

100% tests passed, 0 tests failed out of 18
```

The 18 tests live in two binaries:

- Tests #1–#12 (`AecChain.*`) come from `src/tests/aec_chain_test.cc` and exercise the full chain (Beamformer → AEC3 → NS).
- Tests #13–#18 (`Beamformer.*`) come from `src/tests/beamformer_test.cc` and exercise the collapse stage in isolation.

**Proves:** `AecChain` rejects unsupported sample rates and out-of-range mic counts (per ADR-0003 + ADR-0004), accepts both 16 kHz and 48 kHz, the Beamformer collapses N-channel input to ch[0] verbatim and rejects misshaped frames, real WebRTC AEC3 attenuates correlated echo by **> 15 dB** at both 16 k and 48 k (Task 8 tightening), `webrtc::AudioProcessingStats::echo_return_loss_enhancement` populates after a brief warm-up, RNNoise NS is active at both rates (Speex resamples 16↔48 kHz around it), and the chain measures CPU/audio time correctly (RTF < 1).

### Step C — Generate synthetic test audio

```sh
python3 reference/gen_synth.py --duration 10 --out-dir reference/synth/
```

**Expected:**

```
wrote reference/synth/{ref,mic,near_clean,noise_road,noise_bark,noise_hvac}.wav  (10.0s @ 16000 Hz)
```

This produces six 10-second 16 kHz mono WAVs:

| File | Contents |
|---|---|
| `ref.wav` | White noise — the "far-end" stimulus, what the speaker would play |
| `mic.wav` | `ref.wav` convolved with a 30 ms synthetic IR + a low-amplitude near-end tone |
| `near_clean.wav` | The near-end tone alone (the "oracle" — what AEC should ideally recover) |
| `noise_road.wav` | Pink-ish broadband rumble + low-frequency wobble (~-20 dBFS RMS) — for `--inject-noise` |
| `noise_bark.wav` | 4-8 short band-limited bursts (peak ~-6 dBFS) — for `--inject-noise` |
| `noise_hvac.wav` | 60 Hz hum + harmonics over a low pink floor (~-22 dBFS RMS) — for `--inject-noise` |

Add `--noise-only` to regenerate just the three noise stems without rebuilding the stimulus pair.

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
frames=1000  audio=10.000s  cpu=0.575s  rtf=0.0575  erle_db=16.68  dropped=0
```

**How to read it:**
- `frames=1000` and `audio=10.000s` → the harness consumed all 10 s of the input pair correctly.
- `rtf` ≪ 1.0 → the chain is faster than realtime; on an M-class laptop expect roughly 0.04–0.10 with the real AEC3 + RNNoise stack (the 16 k tier adds Speex 16↔48 kHz resampling around RNNoise; numbers vary).
- `erle_db` is `webrtc::AudioProcessingStats::echo_return_loss_enhancement` surfaced through `ChainStats::echo_return_loss_enhancement_db` (an `std::optional<double>`, per ADR-0006). With real AEC3 (Task 6) it populates after a brief warm-up; the bench prints `N/A` only if the optional is still unset (e.g., the stimulus was so short AEC3 didn't have time to converge). On the synthetic mic/ref pair above, expect a value in the 12–20 dB range — typically around 16–17 dB.
- `dropped=0` — non-zero means a HAL/harness bug fed the chain a frame with wrong rate/channels/samples; should always be zero in healthy runs.

```sh
ls -la /tmp/ecnr_offline_out.wav  # should be ~320 KB (10 s × 16 kHz × 2 bytes)
```

**Proves:** WAV I/O, frame-by-frame chain processing, RTF accounting, and output writing all work end-to-end on deterministic input.

### Step E — Live host loopback (real speaker + real mic) — **requires human-in-the-loop verification**

> This is the one step in the runbook that can't be fully verified non-interactively. macOS will prompt for **Microphone permission** the first time you run `ecnr_live`; you must approve it in System Settings → Privacy & Security → Microphone (then restart your terminal). On second-and-later runs no prompt.
>
> Wear headphones if you don't want the room to hear the stimulus, OR run with the volume low. The stimulus is 10 s of white noise. Don't mute your speakers — the whole point is to drive a real speaker→mic loop so AEC3 has something to cancel.

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

**Listening test (the human-in-the-loop part):**

```sh
afplay /tmp/ecnr_live_out.wav   # macOS
```

You should hear roughly your room ambient noise with the stimulus white-noise echo audibly removed (or near-removed). With the Phase 0.5 backends (real AEC3 + RNNoise) the stimulus should be substantially gone — markedly cleaner than the Phase-0 stub used to produce.

**Phase 0.5 verification:** run this once and listen-test the output. If the echo of the stimulus is audibly removed (or near-removed), Phase 0.5 backend wiring is functioning end-to-end on your hardware. If you still hear most of the white noise, check the prerequisites: speakers actually playing audibly into the mic (not muted, not unplugged), no aggressive OS-level AEC on the input device, and the `erle_db` summary value should be a few dB or higher.

**Proves:** CoreAudio backend works, miniaudio capture + playback callbacks plumb correctly into the ring buffers, the AEC chain processes a live stream without dropping frames, and the captured echo is removed by the real WebRTC AEC3 + RNNoise backends.

### Step E.1 — Demo: A/B with noise injection

The fastest way to convince yourself (or anyone else) that the AEC + NS chain is doing real work is to listen to the **same captured mic stream** before and after the chain processes it. `ecnr_live` supports this directly with `--out-raw` (the "before" — what the chain saw) alongside `--out` (the "after"). Optionally, use `--inject-noise` to software-mix a synthetic background noise WAV into the captured mic stream **before** the AEC chain sees it (Option A: pure software mix in the capture path, **not** played through the speakers). The render reference still carries only the stimulus, so AEC3 treats the injected noise as near-end noise — exactly what RNNoise is supposed to suppress.

First, regenerate the synthetic noise WAVs (Step C produces these by default; if you skipped them, run with `--noise-only` to generate just the noise stems):

```sh
python3 reference/gen_synth.py --duration 10 --out-dir reference/synth/
# or, just the three noise files:
python3 reference/gen_synth.py --duration 10 --out-dir reference/synth/ --noise-only
```

Three example invocations:

```sh
# 1. Baseline: white noise stimulus + speech, no extra background noise.
./build/ecnr_live \
  --stimulus reference/synth/ref.wav \
  --out /tmp/aec_after.wav \
  --out-raw /tmp/aec_before.wav

# 2. With synthetic road noise mixed in (-9 dB so it's clearly audible):
./build/ecnr_live \
  --stimulus reference/synth/ref.wav \
  --inject-noise reference/synth/noise_road.wav \
  --inject-gain-db -9 \
  --out /tmp/aec_after.wav \
  --out-raw /tmp/aec_before.wav

# 3. With dog barks (louder, peaky):
./build/ecnr_live \
  --stimulus reference/synth/ref.wav \
  --inject-noise reference/synth/noise_bark.wav \
  --inject-gain-db -6 \
  --out /tmp/aec_after.wav \
  --out-raw /tmp/aec_before.wav
```

A/B playback recipe (macOS):

```sh
echo "=== BEFORE (raw mic + injected noise) ==="
afplay /tmp/aec_before.wav
echo "=== AFTER (AEC + RNNoise) ==="
afplay /tmp/aec_after.wav
```

**What to expect:** speech (your voice) is preserved in both files; the white-noise stimulus echo present in `_before.wav` is substantially gone in `_after.wav` (that's AEC3 doing its job). Stationary injected noise — road rumble, HVAC hum — is also markedly reduced in `_after.wav` thanks to RNNoise. Bark-like transient noise is harder for RNNoise: a residual click usually survives. That's a known limitation of RNNoise's training distribution; stronger transient suppression is Phase 3's job (the neural post-processor).

- Troubleshooting: if `_before.wav` is silent, mic permission isn't actually granted (System Settings → Privacy & Security → Microphone — restart the terminal after enabling). If `_after.wav` is silent but `_before.wav` has content, the chain over-cancelled (rare; investigate the run's `erle_db` and `chain_dropped` numbers).

### Step E.2 — Deterministic A/B test (no human required)

Step E.1 is great for a quick listen but it depends on a human speaking into the mic each time, which makes parameter sweeps painful and direct numerical comparisons unreliable (different takes, different room state, different volume). Step E.2 closes that gap with a fully deterministic harness: the **same input every run**, so any difference in the bench output is attributable to the chain's parameters, not the speaker or the room.

The harness has three pieces:

1. **A clean voice signal**, either recorded once via `ecnr_live --record-voice` (your real voice) or generated synthetically by `gen_synth.py` (`voice_synth.wav`).
2. **A simulated speaker echo**, produced by `gen_test_input.py` convolving the same `ref.wav` stimulus we already use elsewhere with a short room IR.
3. **Optional noise**, mixed in by `gen_test_input.py` at a chosen dB level (matching the gain semantics of `--inject-gain-db`).

The output is a single mic WAV that `ecnr_bench` can process against `ref.wav`.

```sh
# One-time: record your own voice (or skip this and use voice_synth.wav).
./build/ecnr_live --record-voice reference/synth/voice_recorded.wav --duration 15
# Speak naturally for 15 seconds. No stimulus plays — pure capture.

# Generate a test mic WAV combining voice + simulated echo + road noise.
python3 reference/gen_test_input.py \
    --noise reference/synth/noise_road.wav \
    --noise-gain-db -9 \
    --out reference/synth/test_mic_road.wav

# Run AEC on it.
./build/ecnr_bench \
    --mic reference/synth/test_mic_road.wav \
    --ref reference/synth/ref.wav \
    --out /tmp/test_after_road.wav

# A/B compare.
afplay reference/synth/test_mic_road.wav   # before
afplay /tmp/test_after_road.wav             # after
```

The default voice for `gen_test_input.py` is `reference/synth/voice_recorded.wav` if it exists, otherwise `reference/synth/voice_synth.wav`. So once you record once, every subsequent test uses your real voice automatically — no flag changes needed.

**Parameter sweep example** (helpful for characterizing how the chain behaves as the noise floor changes — exactly the kind of thing the user observed when RNNoise over-suppressed under heavy synthetic noise):

```sh
for gain in -18 -12 -9 -6 -3; do
  python3 reference/gen_test_input.py \
    --noise reference/synth/noise_road.wav \
    --noise-gain-db "$gain" \
    --out "reference/synth/test_mic_road_${gain}.wav"
  ./build/ecnr_bench \
    --mic "reference/synth/test_mic_road_${gain}.wav" \
    --ref reference/synth/ref.wav \
    --out "/tmp/test_after_road_${gain}.wav"
  echo "noise_gain=${gain} dB -> /tmp/test_after_road_${gain}.wav"
done
```

**Why this matters:** this is the harness Phase 2 (cabin characterization) and Phase 3 (neural RES selection per ADR-0007) will both build on. Real cabin recordings replace `voice_synth.wav` and `ref.wav`; everything else stays the same. Deterministic inputs make it possible to compare two model variants — or two AEC3 configurations — side by side without arguing about whether one of the takes was just louder.

**Proves:** end-to-end AEC + NS characterization can run reproducibly without a human in the loop, unblocking parameter sweeps and Phase 3 model selection.

### Step E.3 — Canned 60-second multi-scenario demo (no live voice required)

Steps E / E.1 / E.2 each require something — a human speaking into the mic, or a single synthetic stimulus. Step E.3 closes that gap with a **60-second canned scenario walkthrough** built from real CC0 / public-domain recordings: the chain is exercised across cabin echo, road noise, café babble, music, stadium crowd, and dog-bark transients in one A/B you can hand to anyone.

The demo files come from `reference/noise/`, which is **not committed to git** (the audio files are gitignored). One-time fetch:

```sh
scripts/fetch-noise.sh
```

`fetch-noise.sh` reads `reference/noise/MANIFEST.tsv` (the pinned URL + SHA256 + license per file, mirroring `vendor/MANIFEST.tsv`'s pattern) and:
- auto-downloads the two archive.org sources (CC0 dog bark + public-domain Schumann music) and verifies their SHA256;
- prints **manual-download instructions** for the three Freesound CC0 files (Freesound requires a free account for downloads, even on CC0 sounds; the script tells you exactly which URL goes to which path).

Re-running is idempotent: present files at the pinned SHA256 print `ok` and do nothing. License audit summary is in [`reference/noise/README.md`](reference/noise/README.md).

**Optional but strongly recommended — record your own voices.** The synthetic `voice_synth.wav` and `ref_voice.wav` sound abstract; replacing one or both with a real recording makes AEC's "the caller is gone" moment immediately recognizable.

```sh
# Record yourself as the caller (the voice AEC should remove).
# Speak as if you were on the other end of a call — say something distinctive
# like "Hi, this is [name] calling, how are you?". Looped to fill 60 s if shorter.
./build/ecnr_live --record-voice reference/synth/caller_recorded.wav --duration 60

# Optionally also record yourself as the near-end (the voice AEC should preserve).
# Say something different so you can tell the two voices apart in the demo.
./build/ecnr_live --record-voice reference/synth/voice_recorded.wav --duration 60
```

The composer auto-prefers `caller_recorded.wav` and `voice_recorded.wav` when present, falling back to the synthetic versions when not. The print-timeline output shows which is in use:

```sh
python3 reference/gen_combined_demo.py --print-timeline
# ✓ near_voice     real     reference/synth/voice_recorded.wav   ← your recording
# ✓ ref_voice      real     reference/synth/caller_recorded.wav  ← your recording
# (or)
# ~ near_voice     fallback reference/synth/voice_synth.wav      ← synthetic
# ~ ref_voice      fallback reference/synth/ref_voice.wav        ← synthetic
```

Compose the demo:

```sh
python3 reference/gen_combined_demo.py            # 60 s walkthrough (10 s scenes)
python3 reference/gen_combined_demo.py --short    # 30 s walkthrough (5 s scenes)
```

`--short` is the quick-A/B variant. Same scenes in the same order, half the duration each — easier to listen through end-to-end and to compare side by side when sweeping `--ns-dry-blend` / `--ns-vad-blend` values. Outputs land as `demo_30s_*.wav` so the long and short variants coexist on disk without overwriting each other.

This writes two files (per variant) to `reference/synth/`:

- `demo_{60,30}s_ref.wav` — the far-end stimulus (caller voice, looped). Feed this to `ecnr_bench --ref`.
- `demo_{60,30}s_mic.wav` — the synthesized mic capture: near-end voice + cabin-IR'd echo of the far-end voice + scene noises layered across time. **This is the "before" file** — what the chain receives.

Run it through the chain:

```sh
./build/ecnr_bench \
    --mic reference/synth/demo_60s_mic.wav \
    --ref reference/synth/demo_60s_ref.wav \
    --out /tmp/demo_60s_after.wav
```

A/B playback (macOS):

```sh
afplay reference/synth/demo_60s_mic.wav   # before
afplay /tmp/demo_60s_after.wav            # after
```

**What you should hear** (the timeline `gen_combined_demo.py` prints when run is the canonical reference):

| Time | Scene | Source | What to listen for |
|---|---|---|---|
| 0–10 s | quiet baseline | — | Caller voice (in echo) + your near-end voice. After: caller voice should be substantially gone, leaving near-end. |
| 10–20 s | car interior | Freesound CC0 | Real cabin road / engine rumble. RNNoise should suppress this cleanly — it's stationary and what NS handles best. |
| 20–30 s | café babble | Freesound CC0 | Multi-talker babble + cutlery transients. Hardest stationary-NS case (babble has speech-like spectra); some near-end suppression is expected. |
| 30–40 s | music | archive.org PD | Schumann's *Merry Peasant*, classical orchestral. Stresses ADR-0009 (media-aware AEC) — voice-trained NS may treat music inconsistently. |
| 40–50 s | stadium crowd | Freesound CC0 | Football crowd "Oehh" — high-energy non-stationary. Tests the NS's behaviour on a crowd swell without overshooting. |
| 50–60 s | dog barks | archive.org CC0 | Sparse high-amplitude transients at ~50 s and ~55 s. **Known limitation**: RNNoise leaves a residual click (the README's Phase-3 motivation text calls this out as the reason a neural RES post-filter is needed). |

If any noise file is missing (e.g. you skipped the manual Freesound downloads), the composer falls back to a synthetic version of that scene where one exists, or skips the scene with a clear log line. The demo always runs end-to-end; just with sharper realism the more files you have.

**Proves:** the chain handles a multi-scenario walkthrough across realistic acoustic environments — cabin echo, stationary noise, non-stationary noise, music, transients — in a single deterministic 60-second artifact. No live mic input required, fully reproducible across machines, ready to hand to a stakeholder for A/B listening.

### Step F — Re-fetch vendor (optional, slow)

```sh
scripts/fetch-vendor.sh required   # production baselines only (~5 MB)
# or:
scripts/fetch-vendor.sh            # everything (~450 MB)
```

**Expected (the three required repos plus the rnnoise post-fetch model hook, then `done.`):**

```
  ok   webrtc-audio-processing @ d0569cfa50c1858ee279d77b3fc8870be6902441
  ok   rnnoise @ 70f1d256acd4b34a572f999a05c87bf00b67730d
       rnnoise model already present
  ok   speexdsp @ 7a158783df74efe7c2d1c6ee8363c1e695c71226
done.
```

Re-running is idempotent — already-present repos at the pinned SHA print `ok` and do nothing.

**Proves:** The manifest pinning + clone logic works. Required for Phase 0.5 onwards (when the build actually links against vendored code). Not required for Phase 0/0.6.

---

## CLI reference

Complete list of every flag both binaries accept. Run with `-h` or `--help` for the same info inline.

### `ecnr_bench` — offline file replay

Replays a `--mic` WAV through the AEC + NS chain using a paired `--ref` WAV as the far-end reference. Writes the processed output and prints a one-line stats summary.

| Flag | Argument | Required | Default | Purpose |
|---|---|---|---|---|
| `--mic` | path | yes | — | Mic-capture WAV (16 or 48 kHz mono int16). |
| `--ref` | path | yes | — | Far-end reference WAV (same rate as `--mic`, mono int16). |
| `--out` | path | no | `out.wav` | Output WAV for the chain's processed mono uplink. |
| `--ns-dry-blend` | `<0..1>` | no | `0.0` | **Step A NS cap.** Uniform wet/dry mix on RNNoise's output: `final = α·input + (1−α)·rnnoise_out`. `0.0` = unchanged RNNoise; `0.25` ≈ −12 dB suppression floor; `1.0` = NS bypass. Mitigates chopped-voice artifact on heavy noise. |
| `--ns-vad-blend` | `<low,high>` | no | unset | **Step B VAD-gated NS cap.** Interpolates α between `low` (noise-dominant frames) and `high` (voice-dominant frames) using RNNoise's per-frame voice probability. e.g. `0.0,0.30` = full NS on pure-noise frames, ~−10 dB cap on voice. Supersedes `--ns-dry-blend` when both are passed. |
| `-h`, `--help` | — | no | — | Print usage and exit. |

**Stats line printed at exit** (fields shown depend on what flags ran):

```
frames=<N>  audio=<seconds>  cpu=<seconds>  rtf=<ratio>  erle_db=<dB>  dropped=<frames>
[ns_dry_blend=<α>]                              # only when --ns-dry-blend > 0
[ns_vad_blend=<low>..<high>  last_vad=<p>  last_blend=<α>]   # only with --ns-vad-blend
```

`erle_db` is sourced from `webrtc::AudioProcessingStats::echo_return_loss_enhancement` (an `std::optional<double>`, per ADR-0006). Prints `N/A` only if AEC3 hasn't converged enough to populate it (extremely short stimulus).

**Examples:**

```sh
# Plain bench — current default NS behaviour.
./build/ecnr_bench --mic reference/synth/demo_60s_mic.wav \
                   --ref reference/synth/demo_60s_ref.wav \
                   --out /tmp/demo_after.wav

# Step A: cap NS suppression at -12 dB.
./build/ecnr_bench --mic ... --ref ... --out /tmp/step_a.wav --ns-dry-blend 0.25

# Step B: VAD-gated NS — full RNNoise on noise, capped on voice.
./build/ecnr_bench --mic ... --ref ... --out /tmp/step_b.wav --ns-vad-blend 0.0,0.30

# Parameter sweep on the short 30 s demo.
for blend in 0.0 0.15 0.25 0.40; do
  ./build/ecnr_bench --mic reference/synth/demo_30s_mic.wav \
                     --ref reference/synth/demo_30s_ref.wav \
                     --out "/tmp/sweep_${blend}.wav" --ns-dry-blend "$blend"
done
```

### `ecnr_live` — host live loopback (speakers + mic)

Plays a stimulus through the system speakers, captures live from the default mic, runs the chain in real time. Two modes:

- **Live (default)** — `--stimulus FILE` required. Speakers play it; mic captures; chain processes.
- **Record-only** — `--record-voice FILE` set. No stimulus playback, no chain. Pure capture-to-WAV.

| Flag | Argument | Required | Default | Purpose |
|---|---|---|---|---|
| `--stimulus` | path | live mode | — | WAV played out the speakers (the far-end / render). 16 or 48 kHz mono int16; loops `--inject-noise` if shorter. |
| `--out` | path | no | `live_out.wav` | Processed AEC + NS output WAV. Incompatible with `--record-voice`. |
| `--out-raw` | path | no | unset | Raw captured mic stream (post `--inject-noise` mix, pre-AEC). The "before" track for A/B demos. |
| `--inject-noise` | path | no | unset | WAV mixed into the capture stream **before** AEC sees it (mono int16, same rate as stimulus; loops if shorter). Simulates ambient noise that's not coming from the speaker. AEC has no reference for it; RNNoise does the work. |
| `--inject-gain-db` | float | no | `-12.0` | Gain applied to injected noise (dB) before mixing. |
| `--ns-dry-blend` | `<0..1>` | no | `0.0` | Step A NS cap. See `ecnr_bench` table for semantics. |
| `--ns-vad-blend` | `<low,high>` | no | unset | Step B VAD-gated NS cap. Supersedes `--ns-dry-blend` when both passed. |
| `--duration` | float (s) | no | stimulus duration; `15.0` in `--record-voice` mode | Cap session length. Useful for short auditions and for `--record-voice`. |
| `--record-voice` | path | no | unset | **Record-only mode.** Capture from default mic for `--duration` seconds and write a 16 kHz mono WAV. No stimulus playback, no AEC chain. Incompatible with `--out` and `--inject-noise`. Used to record near-end / caller voice WAVs for the deterministic harness (Step E.2 / E.3). |
| `-h`, `--help` | — | no | — | Print usage and exit. |

**Stats line printed at exit:**

```
frames=<N>  audio=<seconds>  rtf=<ratio>  erle_db=<dB>  cap_dropped=<N>  ref_dropped=<N>  chain_dropped=<N>
[ns_dry_blend=<α>]  or  [ns_vad_blend=<low>..<high>  last_vad=<p>  last_blend=<α>]
```

`cap_dropped` / `ref_dropped` should both be 0 — non-zero means the processing thread starved or a ring buffer overflowed (thread scheduling or device buffer issue). `chain_dropped` is the chain's own shape-mismatch counter; non-zero indicates a HAL / harness bug.

**Examples:**

```sh
# Headline demo: 60 s walkthrough with caller + scene noises through speakers.
./build/ecnr_live --stimulus reference/synth/demo_60s_speaker_mix.wav \
                  --out /tmp/live_after.wav \
                  --out-raw /tmp/live_before.wav

# Short variant (30 s; easier to A/B).
./build/ecnr_live --stimulus reference/synth/demo_30s_speaker_mix.wav \
                  --out /tmp/live_30s_after.wav \
                  --out-raw /tmp/live_30s_before.wav

# Step B VAD-gated NS during the live demo.
./build/ecnr_live --stimulus reference/synth/demo_60s_speaker_mix.wav \
                  --ns-vad-blend 0.0,0.30 \
                  --out /tmp/live_step_b.wav

# Inject-noise mode: speaker plays only the caller; noise is software-mixed
# into the captured mic (chain has no reference for it, NS does the work).
./build/ecnr_live --stimulus reference/synth/demo_60s_ref.wav \
                  --inject-noise reference/synth/demo_60s_noise.wav \
                  --out /tmp/live_inject.wav --out-raw /tmp/live_inject_raw.wav

# Record-only: capture 60 s of your voice for the caller slot in the canned demo.
./build/ecnr_live --record-voice reference/synth/caller_recorded.wav --duration 60
```

**Mode interactions:**

- `--stimulus` + `--inject-noise`: speakers play stimulus, mic mix includes injected noise. Two independent paths AEC must reckon with separately (speaker echo via reference, injected noise via NS only).
- `--stimulus` of a combined file (e.g. `demo_60s_speaker_mix.wav`): speakers play caller + noise together; AEC reference covers both; AEC cancels their composite echo as one signal. Different test condition from `--inject-noise`.
- `--record-voice`: live mode, but no chain. Mutually exclusive with `--out` and `--inject-noise` (and any chain-output flag).
- `--ns-dry-blend` and `--ns-vad-blend` both passed: `--ns-vad-blend` wins; Step B is a strict superset of Step A (uniform blend is the `low == high` case).

### `ecnr_eval` — AEC3 tuning + ERLE measurement harness

The harness behind the AEC3 tuning methodology locked in [ADR-0011](docs/adr/0011-aec3-tuning-methodology.md). Emits **two** ERLE numbers per condition:

- **`erle_reported_*`** — AEC3's self-reported `echo_return_loss_enhancement_db` (operational telemetry, what production logs).
- **`erle_true_*`** — externally computed by feeding the echo-only mic track through a fresh chain instance and RMS-comparing the residual output against the echo-only input. The two-number contract makes AEC3's self-report bias measurable.

**`--self-test`** — in-memory synthetic fixture; asserts `erle_true_db_median > 12`. No file I/O. Used as CI smoke + harness hello-world.

```
$ ./build/ecnr_eval --self-test
ecnr_eval self-test (5 s, 1 kHz tone, gain-0.5 echo, 16 kHz):
  frames processed       = 500
  reported ERLE median   = 0.18 dB (p10=0.18, p90=0.18)
  true ERLE median       = 47.37 dB (p10=18.06, p90=80.00)
  frames used (true)     = 400
  frames skipped settle  = 100
  frames below gate      = 0
PASS
```

The reported-vs-true divergence on this synthetic fixture (~47 dB gap) is expected — AEC3's self-report isn't calibrated against the trivial-linear-echo case. On realistic cabin recordings the two should track within ~2 dB (ADR-0011 open assumption A3).

**`--run --conditions DIR --out FILE.csv`** — sweep mode. Iterates `DIR/*/` subdirectories; each must contain `mic.wav` + `ref.wav` + `echo_only_mic.wav` (all same rate, all same length). Emits per-condition CSV rows. Phase 2 will feed this against the 134-case corpus; today it works against any compatible condition tree.

```
$ ./build/ecnr_eval --run --conditions ./conditions/synthetic --out results.csv
condition case_001_quiet_cabin: 80000 samples @ 16000 Hz
wrote results.csv (1 conditions processed, 0 skipped)

$ head -2 results.csv
condition_id,config_name,sample_rate_hz,erle_reported_median_db,…,frames_skipped_settle_true
case_001_quiet_cabin,default-webrtc,16000,0.176,…,100
```

CSV schema is a strict subset of the locked ADR-0011 §4 contract: the `config_hash` and `condition_hash` columns are deferred until the TOML sweep parser lands. Today there is one config: `default-webrtc`.

The CMake option `ECNR_BUILD_EVAL` (default ON) gates this target; cross-builds set it OFF — the harness is a host-only tool.

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
| `AecChain.AttenuatesCorrelatedEcho` or `…At48k` fails | Real AEC3 wiring regressed below the 15 dB threshold (Task 8) — likely a swap of the AEC3 config, the resampler, or the NS plumbing | Investigate the most recent change to `src/pipeline/`; do not loosen the assertion |
| `meson` / `ninja: command not found` during configure (vendored webrtc-apm fails to build) | The vendored `webrtc-audio-processing` is built with meson at configure time | `brew install meson ninja` (macOS) or `apt install meson ninja-build` (Debian/Ubuntu), then `rm -rf build && cmake -S . -B build …` |

---

## Layout

| Path | Purpose |
|---|---|
| `docs/` | Background research (CN + EN deep-research reports + 5G VoNR PDF) |
| `vendor/` | Pinned upstream open-source deps (gitignored; see `MANIFEST.tsv`) |
| `src/core/` | Frame, ring buffer, resampler |
| `src/pipeline/` | `AecChain` — Beamformer collapse + WebRTC AEC3 + RNNoise NS wire-up (real backends as of Phase 0.5) |
| `src/hal/` | Mic/render abstraction (file-backed via libsndfile for v1) |
| `src/bench/` | `ecnr_bench` — offline file replay harness |
| `src/live/` | `ecnr_live` — live host loopback (miniaudio mic + speaker) |
| `src/tests/` | gtest — `aec_chain_test.cc` (full-chain integration) and `beamformer_test.cc` (collapse-stage unit tests) |
| `models/` | Neural model artifacts (Phase 3+) |
| `reference/` | Test audio + synthesizer (`gen_synth.py`) |
| `scripts/` | Vendor fetcher (`fetch-vendor.sh`) |
| `third_party/` | Vendored single-header deps (miniaudio) and FetchContent doc |

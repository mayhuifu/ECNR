# ECNR — Automotive In-Cabin AEC + NR Audio Stack for U300

## Mission

Build a production audio front-end (acoustic echo cancellation + noise reduction) for the **U300** system software, targeting the **automotive in-cabin** acoustic environment. v1 runs on **Cortex-A55**; Tensilica HiFi (C1 / BX2) DSP offload is a later phase.

## Architecture

Linear AEC backbone (classical DSP) + neural post-processing for residual echo and non-stationary noise. **Not** a pure end-to-end neural replacement — the research consensus (`docs/deep-research-report.md`, `docs/Cellular Audio Processing Solutions Deep Dive.md`) is that delay alignment, clock drift, reference-signal consistency, and double-talk stability dominate production failures, and a hybrid keeps those concerns separable from model concerns.

```
   far-end (RTP / media playback)
        │
        ▼
   [render tap] ────── reference signal ──┐
                                          │
   mic[N] ──► [resample 16k] ──► [linear AEC: WebRTC AEC3] ──► [neural RES] ──► [NS] ──► [AGC] ──► uplink
                                          ▲                       ▲
                                          │                       │
                                  [delay estimator]        [mode controller]
                                                                  │
                                          idle / NS-lite / AEC / AEC+NN
```

### Design choices

- **10 ms frame loop** (WebRTC AEC3 native). 20 ms only for codec/RTP packetization.
- **16 kHz primary sample rate** — best A55 efficiency per the research; 32/48 kHz reserved for premium tier.
- **Single-core affinity + DVFS-friendly batching.** Optimize for *energy per second of valid speech*, not per-frame throughput.
- **Mode controller** switches algorithm depth based on activity (idle / NS-lite / AEC / AEC+NN). Cheaper than static model bloat.
- **Linear AEC stays classical** (DSP-friendly). Neural lives in the post-filter (RES + NS) only.
- **Reference signal must be tapped post-DRC/EQ/speaker-protection**, as close to the speaker driver as possible. Mismatch here is the #1 source of "AEC doesn't work."

### Automotive specifics — known unknowns

The source research is primarily *cellular* (VoLTE/VoNR). The following must be characterized in Phase 2 before tuning the cabin variant:

- Cabin reverb tail (~100–300 ms expected, vs ~50 ms for handsets).
- Road / wind / HVAC non-stationary noise statistics.
- Music / media playback echo (vs voice-only echo on phones).
- Multi-mic geometry (driver vs passenger), beamforming policy, multi-zone.
- ASR placement (upstream or downstream of AEC?).

## Roadmap

| Phase | Goal | Notes |
|---|---|---|
| **0. Bootstrap** | Project doc + vendor manifest + scaffold with **stub** AEC/NS backends + green smoke test on dev host | Done |
| **0.5 Backend wiring** | Replace stubs with real WebRTC AEC3 + RNNoise behind same `AecChain` interface; tighten ERLE assertion to > 15 dB | Next |
| **0.6 Host live E2E** | `ecnr_live` binary using miniaudio: play stimulus through Mac speakers, capture from mac mic, run AecChain live, write recovered output + report measured ERLE. Cross-platform-ready (Mac/Linux/Windows). | Next |
| 1. Baseline tier on A55 | Cross-compile, productize WebRTC AEC3 + RNNoise, A/B vs reference set | |
| 2. Cabin characterization | Measure cabin IR; build road/wind/HVAC + double-talk reference corpus | Vehicle access required |
| 3. Hybrid v1 | Integrate **NKF-AEC** (5.3K params, RTF 0.09) or **DTLN-AEC** as neural RES post-filter | Verify licenses first |
| 4. Mode controller + DVFS | Activity-based depth switching; power profiling | |
| 5. Field validation | In-vehicle regression suite; productization gates | |
| 6. DSP offload (optional) | Migrate linear AEC to HiFi C1/BX2; A55 hosts NN post-filter only | Decision deferred |

## Vendored open-source dependencies

All forks live under `vendor/<repo-name>/`. Vendored as plain clones (not submodules) per the bootstrap decision. The user maintains personal forks on GitHub and we clone those.

| Upstream | Tier | License | Purpose |
|---|---|---|---|
| `gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing` | **Production baseline** | BSD-3 + PATENTS | Linear AEC3 + NS + AGC; main backbone (canonical C++ source used by PulseAudio/PipeWire) |
| `github.com/xiph/rnnoise` | **Production baseline** | BSD-3 | Lightweight NS post-filter |
| `github.com/xiph/speexdsp` | Secondary baseline | BSD-3 (revised) | Alternative AEC for low-power tier; resampler |
| `github.com/Rikorose/DeepFilterNet` | Research / future | MIT + Apache-2.0 (dual) | 48 kHz full-band NS option |
| `github.com/breizhn/DTLN-aec` | Research / future | MIT (verify) | Neural AEC reference (Python + TF, pretrained models) |
| `github.com/fjiang9/NKF-AEC` | Research / future | Verify license | 5.3K-param Kalman-NN hybrid; key A55 candidate |
| `github.com/athena-team/athena-signal` | Research / RES ref | Apache-2.0 (verify) | Multi-mic, DTD, ERLE, RES research |

**Status:** All seven cloned upstream-direct (no user fork) on 2026-05-09 to unblock Phase 0. Forks can be wired in later by replacing `vendor/<repo>` with a clone of the user's fork; nothing in our integration code depends on the remote URL.

**License verification** is required before any code derived from a "verify" entry ships in a binary. Vendoring (cloning into `vendor/`) does not by itself create a derivative.

## Repository layout

```
ECNR/
├── CLAUDE.md                # guidance for Claude Code instances
├── PROJECT.md               # this file
├── README.md                # build & run instructions
├── docs/                    # research reports + 5G VoNR PDF
├── vendor/                  # cloned forks, untouched
├── src/
│   ├── core/                # frame, ring buffer, resampler
│   ├── pipeline/            # AEC + NS + AGC orchestration
│   ├── hal/                 # mic/render abstraction (file-backed for v1)
│   ├── bench/               # offline benchmark harness
│   └── tests/               # gtest
├── models/                  # neural model artifacts (TFLite / ONNX) — Phase 3+
├── reference/               # test audio (cabin IRs, road noise, double-talk corpus)
├── third_party/             # gtest, libsndfile (system or downloaded)
└── CMakeLists.txt
```

## Decisions log

- **2026-05-09** — Cortex-A55 only for v1; HiFi C1/BX2 DSP offload deferred (Phase 6 if at all).
- **2026-05-09** — Vendor open-source as plain clones (no submodules). User forks deferred — Phase 0 clones upstream directly to unblock.
- **2026-05-09** — WebRTC source = canonical `gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing` (pure C++ + Meson). Earlier candidate `tonarino/webrtc-audio-processing` rejected because it's a Rust wrapper around a submodule of the same freedesktop source.
- **2026-05-09** — WebRTC AEC3 + RNNoise as the baseline pair. NKF-AEC is the leading neural RES candidate based on RTF 0.09 / 5.3K params.
- **2026-05-09** — 16 kHz primary sample rate; 10 ms frame.
- **2026-05-09** — Project repo lives at `/Users/huifu/Project/ECNR`. Decision on remote (GitHub/GitLab) deferred.
- **2026-05-09** — `vendor/` source is **not** committed to git (~451 MB; mostly DeepFilterNet + DTLN-aec pretrained models). Pinned by `vendor/MANIFEST.tsv` with upstream URL + commit SHA, fetched on demand via `scripts/fetch-vendor.sh`.
- **2026-05-09** — Phase 0 ships **stub** AEC + NS backends behind the `AecChain` interface to unblock the architecture and harness. Real WebRTC AEC3 + RNNoise wiring is its own milestone (Phase 0.5) — defers Meson + autotools build orchestration to a focused effort with visible scaffold progress already on the trunk.
- **2026-05-09** — Add Phase 0.6 (host live E2E on macOS) using **miniaudio** (single-header, public-domain/MIT, cross-platform). Vendored at `third_party/miniaudio/miniaudio.h`. Picked over PortAudio for zero-dep build and over CoreAudio for cross-platform reach (Linux ALSA / Windows WASAPI come for free).
- **2026-05-09** — Project published to https://github.com/mayhuifu/ECNR (public).

## Open questions

- U300 audio HAL integration API — callback vs pull/push, threading model, where the render tap lives in the existing pipeline. Required before Phase 1.
- Vehicle access for cabin acoustic characterization. Required before Phase 2.
- Whether to ship under a single namespace (e.g. `ecnr::`) or align with U300 conventions. Defer to Phase 1.
- Final license verification for DTLN-AEC and NKF-AEC. Required before Phase 3.

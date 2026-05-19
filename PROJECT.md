# ECNR — Automotive In-Cabin AEC + NR Audio Stack for U300

## Mission

Build a production audio front-end (acoustic echo cancellation + noise reduction) for the **U300** system software, targeting the **automotive in-cabin** acoustic environment. v1 runs on **Cortex-A55**; Tensilica HiFi (C1 / BX2) DSP offload is a later phase.

## Architecture

Linear AEC backbone (classical DSP) + neural post-processing for residual echo and non-stationary noise. **Not** a pure end-to-end neural replacement — the research consensus (`docs/deep-research-report.md`, `docs/Cellular Audio Processing Solutions Deep Dive.md`) is that delay alignment, clock drift, reference-signal consistency, and double-talk stability dominate production failures, and a hybrid keeps those concerns separable from model concerns.

```
   far-end (RTP / media playback)
        │
        ▼
   [render tap] ────────── reference signal ─────────────┐
                                                         │
   mic[N=2..8] ──► [resample] ──► [Beamformer N→1] ──► [linear AEC: WebRTC AEC3] ──► [neural RES] ──► [NS] ──► [AGC] ──► uplink
                                          ▲                     ▲                        ▲
                                          │                     │                        │
                                   geometry hint        [delay estimator]         [mode controller]
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
| **0.5 Backend wiring** | Real WebRTC AEC3 + RNNoise wired behind the existing `AecChain` interface; multi-rate (16/48 kHz) and 2–8 mic support via a stub `Beamformer` (per ADR-0003 + ADR-0004); ERLE assertion tightened to > 15 dB on the synthetic correlated-echo test (measured ~64 dB at 16 k / ~62 dB at 48 k); RTF ~0.06 on macOS Apple Silicon. | Done |
| **0.6 Host live E2E** | `ecnr_live` binary using miniaudio: play stimulus through Mac speakers, capture from mac mic, run AecChain live, write recovered output + report measured ERLE. Cross-platform-ready (Mac/Linux/Windows). | Done |
| **1. Baseline tier on A55** | Cross-compile, productize WebRTC AEC3 + RNNoise, A/B vs reference set | **In flight (current)** — see status below |
| 2. Cabin characterization | Measure cabin IR; build road/wind/HVAC + double-talk reference corpus | Vehicle access required |
| 3. Hybrid v1 | Integrate **NKF-AEC** (5.3K params, RTF 0.09) or **DTLN-AEC** as neural RES post-filter | Verify licenses first |
| 4. Mode controller + DVFS | Activity-based depth switching; power profiling | |
| 5. Field validation | In-vehicle regression suite; productization gates | |
| 6. DSP offload (optional) | Migrate linear AEC to HiFi C1/BX2; A55 hosts NN post-filter only | Decision deferred |

### Phase 1 status (current: 2026-05-19, tag `v0.2`)

What's **landed**:

- ✅ DSB beamformer + `MicGeometry` ([ADR-0010](docs/adr/0010-mic-geometry-and-beamforming.md), tag `v0.1`)
- ✅ Cross-compile to aarch64-poky-linux via Yocto SDK in Docker ([ADR-0001 A7](docs/adr/0001-hybrid-aec-architecture-review.md), `scripts/cross-build-yocto/`)
- ✅ Cross-build aarch64 perf + size optimization (qemu smoke, Release + strip, RNNoise int8 path — 17.7 MB → 4.25 MB, 2.8× faster, tag `v0.2`)
- ✅ AEC3 tuning methodology + `ecnr_eval` harness ([ADR-0011](docs/adr/0011-aec3-tuning-methodology.md), v0.1)
- ✅ NS over-suppression mitigations — Steps A + B (`--ns-dry-blend`, `--ns-vad-blend`)
- ✅ Multi-rate (16 / 48 kHz) + multi-mic (2..8) contract via `Frame` (Phase 0.5, ADRs 0003 + 0004)
- ✅ Bench `--out-raw` for sample-aligned A/B listening

What's **remaining** in Phase 1 before declaring closeout:

- ⏳ **AGC stage** — chain currently outputs at −38 to −42 dBFS vs 3GPP target −20 to −16 (see Known limitations below). WebRTC AGC2 flag-flip; ~half-day.
- ⏳ **A/B vs reference set** — `ecnr_eval --run` works against any condition tree, but the Phase 1 deliverable was "A/B vs reference set" and we haven't published a formal Phase-1 reference run yet.
- ⏳ **Listening verdict on Move B (int8 RNNoise NS path)** — quantitative voice-RMS deltas are <0.3 dB across blends, but perceptual A/B is still queued.
- ⏳ **TOML sweep parser for `ecnr_eval`** — locks `config_hash` / `condition_hash` columns per ADR-0011 §4.

What's **blocked on external inputs** (Phase 1 cannot fully close until these):

- 🚧 Real A55 hardware OR the U300 vendor SDK — current cross-build runs against a stand-in Poky reference SDK and measures perf under qemu (≈5–10× overhead). The "measure DSB CPU on A55" action item from [ADR-0001 A7](docs/adr/0001-hybrid-aec-architecture-review.md) needs one of these to close.
- 🚧 Phase 2 cabin recordings — required for Step C (AEC3 per-condition tuning) and ADR-0012 (near-end damage metric). Both are gated on Phase 2, which is itself gated on vehicle access.

## Vendored open-source dependencies

All forks live under `vendor/<repo-name>/`. Vendored as plain clones (not submodules) per the bootstrap decision. The user maintains personal forks on GitHub and we clone those.

| Upstream | Tier | License | Purpose |
|---|---|---|---|
| `gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing` | **Production baseline** | BSD-3 + PATENTS | Linear AEC3 + NS + AGC; main backbone (canonical C++ source used by PulseAudio/PipeWire) |
| `github.com/xiph/rnnoise` | **Production baseline** | BSD-3 | Lightweight NS post-filter |
| `github.com/xiph/speexdsp` | Secondary baseline | BSD-3 (revised) | Alternative AEC for low-power tier; resampler |
| `github.com/Rikorose/DeepFilterNet` | Research / future | MIT + Apache-2.0 (dual) | 48 kHz full-band NS option |
| `github.com/breizhn/DTLN-aec` | Research / future | MIT | Neural AEC reference (Python + TF, pretrained models) |
| `github.com/fjiang9/NKF-AEC` | Research / future | BSD-3-Clause | 5.3K-param Kalman-NN hybrid; key A55 candidate |
| `github.com/athena-team/athena-signal` | Research / RES ref | Apache-2.0 | Multi-mic, DTD, ERLE, RES research |

**Status:** All seven cloned upstream-direct (no user fork) on 2026-05-09 to unblock Phase 0. Forks can be wired in later by replacing `vendor/<repo>` with a clone of the user's fork; nothing in our integration code depends on the remote URL.

**License audit (2026-05-10):** all four "verify" entries cleared. NKF-AEC carries its BSD-3 declaration in source-file headers (Tencent / THL A29 Limited, 2022) rather than a top-level LICENSE file — verified by inspecting `vendor/nkf-aec/src/nkf.py`. DTLN-AEC, athena-signal, DeepFilterNet all confirmed via top-level license files. Every vendored dependency is now MIT, BSD-3, or Apache-2.0 — all commercial-friendly, no GPL contamination, no copyleft surprises. Vendoring (cloning into `vendor/`) does not by itself create a derivative; the audit was completed as a Phase-3 prerequisite per ADR-0007 O3.

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
- **2026-05-10** — ADR-0003 accepted: two-tier sample rate (16 kHz baseline + 48 kHz fullband); rate set at `AecChain::Init`; both tiers use 10 ms frames.
- **2026-05-10** — ADR-0004 accepted: 2-8 mics, runtime-configured; new `Beamformer` chain stage upstream of AEC3 (stub passes ch[0] for Phase 0.5; real algorithm gated by ADR-0010).
- **2026-05-10** — ADR-0005 accepted: render-tap policy (post-DRC/EQ/protection, as close to the speaker driver as the platform allows).
- **2026-05-10** — Phase 0.5 Stage 0 ADRs locked: ADR-0003 (two-tier 16/48 kHz), ADR-0004 (2–8 mic dynamic config with stub Beamformer in Phase 0.5; real beamforming gated on ADR-0010), ADR-0005 (render tap post-software-DRC/EQ, pre-hardware-amp), ADR-0006 (`AecChain` interface alignment with WebRTC APM — `std::optional<double>` for stats, opaque pimpl boundary for vendor types).
- **2026-05-10** — `Frame` refactored to rate-aware (`n_samples`) + channel-aware (`n_channels`, `ch[kMaxMics][kFrameSamples48k]`). Per-frame storage 7.7 KB, stack-allocatable, no audio-thread allocations. Active subrange semantics documented in `core/frame.h`.
- **2026-05-10** — WebRTC AEC3 wired via `Aec3Adapter` (opaque pimpl; no `webrtc::` types appear in any public header). The vendored `webrtc-audio-processing` is built via Meson `ExternalProject` with a macOS CoreFoundation framework workaround for the upstream's missing link in `examples/run-offline`.
- **2026-05-10** — RNNoise wired via `RnNsAdapter` (opaque pimpl). Inline-built from upstream `RNNOISE_SOURCES` (10 explicit `.c` files, NOT a wildcard glob — globbing would have pulled in training tools). The 16 kHz tier resamples via SpeexDSP `speex_resampler_process_int` (quality 5) around RNNoise's native 48 kHz / 480-sample frames.
- **2026-05-10** — Stub `Beamformer` lives at `src/pipeline/beamformer.h` and selects `ch[0]` verbatim. Real beamforming (delay-and-sum / MVDR / GSC) deferred to ADR-0010 + Phase 1.
- **2026-05-10** — `frames_dropped` field on `ChainStats` surfaces shape-mismatch rejections instead of silent no-ops. Bench/live binaries print the counter; non-zero indicates a HAL/harness bug.
- **2026-05-10** — Test threshold for cumulative ERLE tightened to > 15 dB (real AEC3 measures ~64 dB at 16 kHz / ~62 dB at 48 kHz on the synthetic correlated-echo stimulus). RTF measured ~0.057 on macOS Apple Silicon.
- **2026-05-10** — Phase 0.5 closed out (Task 10): real WebRTC AEC3 + RNNoise + multi-rate + multi-mic Frame + > 15 dB ERLE thresholds all landed; 18/18 tests green; only the user's interactive listening test for `ecnr_live` remains as a manual verification step.
- **2026-05-10** — ADRs 0007/0008/0009/0010 accepted in parallel after Phase 0.5 ship. Resolves the four open ADRs from ADR-0001's action items: TFLite + XNNPACK as neural runtime, six-trigger DSP-offload criteria (Phase 6 still deferred), HAL-supplied `RenderType` hint for media-aware AEC, fixed delay-and-sum beamformer with explicit `MicGeometry` config for Phase 1.
- **2026-05-10** — License audit cleared all four "verify" markers. NKF-AEC is BSD-3-Clause (Tencent / THL A29 Limited, 2022; declaration in source-file headers). DTLN-AEC is MIT, athena-signal is Apache-2.0, DeepFilterNet is MIT + Apache-2.0 dual. All vendored deps are commercial-friendly. Phase-3 prerequisite per ADR-0007 O3 cleared.
- **2026-05-10** — ADR-0002 (cabin reverb tail) opened as a stub. Numbering complete (0001–0010 all on disk). Decision deferred to Phase 2 when cabin IR measurements arrive; AEC3 default config holds in production until then.

## Open questions

- U300 audio HAL integration API — callback vs pull/push, threading model, where the render tap lives in the existing pipeline. Required before Phase 1.
- Vehicle access for cabin acoustic characterization. Required before Phase 2.
- Whether to ship under a single namespace (e.g. `ecnr::`) or align with U300 conventions. Defer to Phase 1.
- ~~Final license verification for DTLN-AEC and NKF-AEC. Required before Phase 3.~~ — Cleared 2026-05-10 (audit committed).

## Known limitations (deferred work)

- **RNNoise over-suppression on non-stationary scenes (observed 2026-05-10).** During the canned live demo (`ecnr_live --stimulus reference/synth/demo_60s_speaker_mix.wav` — speakers playing caller voice + cafe babble / stadium crowd / music / dog barks; user speaks live; chain output captured at `/tmp/live_after.wav`), the near-end voice in the after file is occasionally chopped / warbly. Classic RNNoise over-aggressive-NS artifact: when the noise floor rises sharply or the noise shares speech-like spectra (babble), RNNoise's per-band mask drops bands that also carry voice content. End-to-end resolution path is **Phase 3** (neural RES post-filter — NKF-AEC / DTLN-AEC per [ADR-0007](docs/adr/0007-neural-runtime.md)) which selectively distinguishes voice from noise better than RNNoise's general-purpose model. **Three cheaper interim mitigations** that can land before Phase 3:
  1. **Cap NS attenuation floor.** Wet/dry blend in `RnNsAdapter`: final = α·input + (1−α)·rnnoise_output with α ≈ 0.25 caps maximum suppression at roughly −12 dB regardless of how aggressive RNNoise's mask gets. Cheapest implementation (~20 lines in the adapter, no FFT or vendor patch needed).
  2. **Bypass NS during clean conditions.** Gate NS invocation on a VAD signal — WebRTC APM's built-in VAD is free since we already run AudioProcessing. When VAD says "speech-dominant frame," reduce NS aggressiveness (or skip it); when "noise-dominant frame," apply full NS. This is the "mode controller" from the architecture diagram in its simplest form (idle / NS-lite / AEC / AEC+NN switching).
  3. **Reduce input level to NS via AEC tuning.** Lower AEC3 residual means NS has less to do and over-suppresses less. Not a single change — an ongoing tuning effort. Most useful after Phase 2 cabin measurements provide ground-truth ERLE numbers per condition.

- **Missing AGC stage — chain output is under-level for uplink (observed 2026-05-18).** [ADR-0001](docs/adr/0001-hybrid-aec-architecture-review.md) calls the architecture `linear AEC → neural RES → NS → AGC`. We have AEC + NS today; the AGC stage isn't wired up. Observed during a bench A/B on `reference/mixed_sound.wav` (input at −31 dBFS RMS, far below typical speech −15 to −20): after AEC3 isolated the near-end voice from the louder echo, the output landed at −38 to −42 dBFS depending on blend — playable via afplay (which is verbatim) but the user reported it sounded "really low" compared to macOS Music (which auto-normalizes). For real-device deployment, 3GPP VoLTE/VoNR uplink expects ~−20 to −16 dBFS RMS, so AGC is needed eventually regardless. **Resolution path:** add WebRTC AGC2 (a flag in the existing `AudioProcessingBuilder` we already use for AEC3 — no new dep), tune target RMS in line with 3GPP recommendations. Estimated ~half-day's work. Quick interim hack if needed for listening tests pre-AGC: apply a fixed gain in `ecnr_bench` before writing `--out` (10 LOC).

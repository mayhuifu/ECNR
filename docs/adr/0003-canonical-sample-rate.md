# ADR-0003: Canonical sample rate(s) — multi-tier (16 kHz baseline + 48 kHz fullband)

**Status:** Accepted
**Date:** 2026-05-10
**Supersedes assumption A2 in:** [ADR-0001](0001-hybrid-aec-architecture-review.md)

## Decision

Two-tier sample-rate strategy:

| Tier | Rate | Frame | Use case |
|---|---|---|---|
| Baseline | 16 kHz | 160 / 10 ms | Voice-only; AMR-WB / EVS-WB; lowest A55 power |
| Fullband | 48 kHz | 480 / 10 ms | EVS-FB; AEC against music / media; hi-fi teleconference |

Rate set at `AecChain::Init(sample_rate_hz)`; both tiers use 10 ms frames. 32 kHz (SWB) supported only on customer demand. 8 kHz dropped.

## Why

A 16 kHz pipeline cannot cancel music played through the same speaker (content above 8 kHz leaks to uplink). U300 ships hands-free during media playback → 48 kHz required. 16 kHz retained as the cheaper voice-only tier (FIR-tap cost scales linearly with rate; per `Cellular Audio Processing Solutions Deep Dive.md:35`, a 100 ms tail at 48 kHz needs 4,800 taps vs 800 at 8 kHz).

Evidence — BdSound (`Cellular Audio Processing Solutions Deep Dive.md:175, :182`): "BdSound AEC algorithms operate at sample frequencies up to 48 kHz, perfectly aligning with the 3GPP EVS Fullband specifications."

## Codebase impact

`src/core/frame.h` — replace `kSampleRateHz` with rate-aware `Frame`:

```cpp
inline constexpr int kFrameDurationMs = 10;
inline constexpr int kFrameSamples16k = 160;
inline constexpr int kFrameSamples48k = 480;

struct Frame {
  int n_samples = kFrameSamples16k;
  std::array<int16_t, kFrameSamples48k> samples{};
};
AecChain::Init validates {16000, 48000}. Mismatched render-vs-capture rate is a programming error → assert. RNNoise stays 48 kHz native: 16 kHz tier pays 16↔48 Speex resampling, 48 kHz tier runs RNNoise inline.

Action items
done
Phase 0.5 plan: Task 6/7 accept rate at Init. 16 kHz default for first wiring; 48 kHz lands as Phase 0.7.
not done
frame.h refactor (single commit).
not done
Bench + live: accept either rate at input WAV; pick matching rate at chain init.
not done
Phase 1: A55 power measurement, 16 vs 48 kHz, with default and reduced AEC3 tails.
Open caveats
"Standard / hi-fi" framing is BdSound's positioning per local research; direct vendor-page confirmation deferred. Decision does not depend on marketing nomenclature.

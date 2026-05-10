
ADR-0004: Microphone array — support 2 to 8 mics dynamically
Status: Accepted
Date: 2026-05-10
Supersedes assumption A3 in: ADR-0001

Decision
Support 2 to 8 microphones dynamically. Mic count is configured at AecChain::Init(sample_rate_hz, num_mics) — runtime state, not compile-time. The chain ingests per-mic capture frames and emits a single uplink frame post-beamform-post-AEC.

Pipeline shape:

mic[N=2..8] → [resample] → [beamformer] → [linear AEC: AEC3] → [neural RES] → [NS] → [AGC] → uplink
                                ▲                ▲                                  ▲
                          geometry hint    render-tap (ref)                  mode controller
The beamformer is a new chain stage upstream of AEC3. AEC3 itself remains single-channel (operates on the post-beamform signal); this matches WebRTC APM's design and avoids per-channel AEC contention.

Why
Cellular research is single-mic; automotive cabins are not. BdSound's Microphone Bubbles (Cellular Audio Processing Solutions Deep Dive.md:25, :175, :182) and NXP VoiceSeeker (:179: "geometry-agnostic beamforming") both ship multi-mic as a first-class feature. In-cabin reasons:

Driver/passenger isolation — without spatial filtering, all mics pick up all speakers.
Road / wind / HVAC noise rejection — non-stationary, directional; multi-mic null-steering is the cheapest tool.
Per-zone tuning — different cabin acoustics by mic position; one stack must fit 2-mic dashboard arrays through 8-mic per-zone arrangements.
Variable count (2–8) rather than fixed N because U300 hardware variants will ship different mic counts (entry trim 2 mics, premium 6–8). One binary, one chain, runtime configuration.

Codebase impact (substantial)
Frame becomes multi-channel:

inline constexpr int kMaxMics = 8;

struct Frame {
  int n_samples  = kFrameSamples16k;
  int n_channels = 1;
  std::array<std::array<int16_t, kFrameSamples48k>, kMaxMics> ch{};   // ch[c][s]
};
Per-channel arrays rather than interleaved — beamformer + per-channel APIs consume pointer-per-channel shapes (matches WebRTC APM's float* const* too). Storage: 8 × 480 × 2 = 7.7 KB per frame; stack-allocatable, cache-friendly.

AecChain::Init(int sample_rate_hz, int num_mics):

num_mics ∈ [2, 8]. Reject 1 (degenerate beamformer; out of scope) and >8.
Allocates beamformer + AEC3 once.
ProcessRender(const Frame&): render is single-channel (the speaker reference). Assert n_channels == 1.

ProcessCapture(const Frame& mic_in, Frame& uplink_out): mic_in.n_channels == num_mics, uplink_out.n_channels == 1. Beamformer collapses N→1 before AEC3.

New stage: Beamformer in src/pipeline/beamformer.{h,cc}. Phase 0.5 ships a stub passthrough that picks ch[0] as the output (so 2-mic and 8-mic configurations all reduce to "use the primary mic"). Real beamforming algorithms (delay-and-sum, MVDR, GSC) are Phase 1+ — gated by ADR-0010 (mic geometry & beamforming algorithm).

Phase 0.5 plan amendments
New Task 5.5: multi-channel Frame + stub beamformer. Lands before Task 6. Single commit. Tests: Frame shape; Beamformer::Process passes ch[0] through; AecChain::Init(rate, num_mics) accepts [2, 8] and rejects 1 / 9.
Tasks 6/7 stay single-channel internally (post-beamform). Ship the stub beamformer so the chain's call-shape is multi-channel from the start.
Phase 1+: ADR-0010 (mic geometry, beamforming algorithm choice) and the real beamformer implementation.
Action items
done
Phase 0.5 plan amendment: add Task 5.5; Tasks 6/7 ride on top.
not done
frame.h refactor for n_channels + per-channel arrays.
not done
Stub Beamformer interface + passthrough impl.
not done
AecChain::Init(rate, num_mics) signature + range check.
not done
Open ADR-0010 — mic geometry & beamforming algorithm — before Phase 1.

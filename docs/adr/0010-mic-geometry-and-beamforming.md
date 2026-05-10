# ADR-0010: Mic geometry hint + beamforming algorithm — Phase 1 first cut: delay-and-sum

**Status:** Accepted (provisional — implementation gated on Phase 1)
**Date:** 2026-05-10
**Supersedes assumption A3 in:** [ADR-0001](0001-hybrid-aec-architecture-review.md); resolves the `TODO(ADR-0010)` in [`src/pipeline/beamformer.h:20-22`](../../src/pipeline/beamformer.h).
**Builds on:** [ADR-0004](0004-mic-array-2-to-8.md) (chain shape locked: per-mic `Frame::ch[]`, `Beamformer` upstream of AEC3, single-channel post-beamform).

## Context

ADR-0004 committed the chain to a multi-mic input shape and locked the `Beamformer` stage as a separate pipeline node upstream of AEC3, but explicitly deferred two questions to this ADR:

1. **Which beamforming algorithm** ships in the first non-stub `Beamformer` implementation?
2. **How does mic geometry information enter the chain** so that algorithm has the steering data it needs?

Phase 0.5 ships a stub at [`src/pipeline/beamformer.cc`](../../src/pipeline/beamformer.cc) that selects `mic_in.ch[0]` verbatim. This is fine for chain plumbing — AEC3 sees mono, the rest of the chain runs unchanged — but it provides zero spatial gain and zero passenger-isolation. It also lets the bench/live binaries get away with mono-duplicating their input into `ch[0]` and `ch[1]` (a Phase-0.5 placeholder, also flagged in ADR-0004): two perfectly correlated channels are degenerate input for any real beamformer (zero inter-mic delay, singular covariance under MVDR).

This ADR resolves both questions decisively but conservatively. It does **not** break the locked `Beamformer` interface from ADR-0004; it adds (a) a `MicGeometry` struct, (b) an extended `Init(...)` overload, and (c) a Phase 1 algorithm choice.

## Decision

### 1. Phase 1 first algorithm: **fixed delay-and-sum (DSB)** with a configurable steering direction.

Phase 1 ships a fixed (non-adaptive) **delay-and-sum** beamformer. Per-mic delays are computed once at `Init` from the supplied `MicGeometry` (mic positions + target direction) and the speed of sound; the runtime path is `N` fractional-delay-aligned channels summed with `1/N` weighting. No covariance estimation, no adaptation, no DOA tracking.

Rationale:
- **Zero adaptation** — converges instantly, behaves identically every frame, deterministic for AEC3 downstream. Adaptive beamformers (MVDR, GSC) interact with AEC3's own adaptation in ways that are hard to reason about until both stages have ground-truth measurements.
- **3 dB SNR improvement at N=2** under spatially-uncorrelated noise, scaling to ~9 dB at N=8 — a meaningful step over the Phase 0.5 stub even before any adaptive algorithm lands.
- **Cheap on A55** — `O(N · frame_samples)` per frame, well under 1% of the Phase 1 CPU envelope. Comfortably below AEC3's cost.
- **Survives bad geometry gracefully** — mis-pointed DSB still produces a sane mono output; mis-pointed MVDR can null the source and amplify noise.
- **Production precedent** — DSB is the "fixed beamformer" half of GSC in athena-signal's [`dios_ssp_gsc/dios_ssp_gsc_filtsumbeamformer.h`](../../vendor/athena-signal/athena_signal/kernels/dios_ssp_gsc/dios_ssp_gsc_filtsumbeamformer.h); shipping DSB first is also the path to GSC later (the FBF stage is reusable).

MVDR / GSC are deferred to **Phase 1.5** (see Phase 1 entry criteria), gated on having (a) a measured cabin noise field, (b) the U300 mic geometry confirmed, and (c) a regression suite that catches MVDR's failure modes (singular covariance, target cancellation under steering error).

### 2. Geometry hint mechanism: **explicit `MicGeometry` struct passed at `Init`.**

Mic positions enter through a **separate, additive `Init` overload**, not through any runtime side channel and not through hardcoded U300 constants. Rationale:

- Hardcoded U300 geometry would couple the beamformer to a single hardware variant — the same binary needs to ship across U300 trim levels with 2–8 mics (ADR-0004).
- DOA estimation is geometry-agnostic but slow to converge and adds a dependency we can't justify before Phase 1.5.
- An explicit struct lets the HAL (or a config file, or a unit test) supply geometry once at startup. NXP VoiceSeeker's "geometry-agnostic" claim ([Cellular Audio Processing Solutions Deep Dive.md:179](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md)) is implemented the same way: linear / triangular / circular arrays with 2–8 cm spacing, all configured by the integrator.

### 3. The `MicGeometry` struct (canonical, copy this into Phase 1 code)

Defined in a new header `src/pipeline/mic_geometry.h`:

```cpp
#pragma once

#include <array>

#include "core/frame.h"  // for kMaxMics

namespace ecnr {

// Cartesian mic-array geometry hint passed to Beamformer::Init.
//
// Frame of reference: arbitrary, but consistent — typically the cabin's
// "driver-forward" frame, with +x = forward (toward windshield), +y = left
// (toward driver in LHD), +z = up. Units: meters. Origin is anywhere
// convenient (often the geometric centroid of the mic array).
//
// Only the first num_mics entries of positions_m are read; the rest are
// ignored. num_mics must match Beamformer::Init(... , num_mics, ...).
struct MicGeometry {
  // (x, y, z) in meters for each mic, in the same channel order as
  // Frame::ch[c]. ch[0] is the reference mic (zero-delay channel for DSB).
  std::array<std::array<float, 3>, kMaxMics> positions_m{};

  // Unit vector pointing from the mic array toward the source of interest
  // (e.g., the driver's mouth). Default: +x (forward). The beamformer
  // steers a beam in this direction; off-direction signals are attenuated.
  std::array<float, 3> target_direction{1.0f, 0.0f, 0.0f};

  // Speed of sound, m/s. Default: 343 (20 C, dry air). Cabin temperature
  // varies; precise value is not load-bearing for DSB at typical mic
  // spacings (sub-sample delay error at 16 kHz / 4 cm / +-20 C).
  float speed_of_sound_mps = 343.0f;
};

// Convenience: a "no spatial information" geometry. Beamformer::Init with
// this falls back to selecting ch[0] (Phase 0.5 stub behaviour). Useful for
// HAL bring-up, unit tests, and the bench/live --bypass-beamformer mode.
constexpr MicGeometry kPassthroughGeometry{};

}  // namespace ecnr
```

The struct is intentionally minimal. **Not yet included** (deferred to Phase 1.5 when the algorithm needs them):

- **Multiple constraint directions** (LCMV-style "preserve driver, null passenger"). Adding `std::array<std::array<float,3>, K> null_directions` is a backward-compatible field add when the time comes.
- **Per-mic gain / sensitivity calibration.** VoiceSeeker explicitly tolerates mismatched mics ([Cellular Audio Processing Solutions Deep Dive.md:154](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md)); for DSB, channel mismatch produces only a small SNR penalty, not a failure.
- **Wind direction / noise field model.** DSB doesn't use it; MVDR/GSC will need a noise covariance estimator, which is internal state, not a geometry input.

### 4. Updated `Beamformer::Init` signature (additive only)

The existing `bool Init(int sample_rate_hz, int num_mics)` from ADR-0004 stays. A second overload is added:

```cpp
// Existing (ADR-0004): geometry-less init, retained for the stub
// passthrough path. Behaves identically to Init(rate, num_mics,
// kPassthroughGeometry).
bool Init(int sample_rate_hz, int num_mics);

// New (this ADR): geometry-aware init. Required for any non-passthrough
// beamforming algorithm (DSB / MVDR / GSC). Returns false if num_mics is
// out of range, geometry has degenerate positions (e.g., two mics at the
// same coordinate), or target_direction is the zero vector.
bool Init(int sample_rate_hz, int num_mics, const MicGeometry& geometry);
```

The two-arg overload is preserved so Phase 0.5 callers (and any HAL bring-up path that doesn't yet know its geometry) compile unchanged. Internally, the two-arg form forwards to the three-arg form with `kPassthroughGeometry`, and the implementation detects that case and selects the `ch[0]` passthrough fast path.

**Algorithm choice is an internal detail of `Beamformer`** — not part of the public interface. Switching DSB → MVDR → GSC in Phase 1.5 is a re-implementation behind the same `Init`/`Process`/`Reset` surface.

### 5. Bench/live harness fix — option B + option A combined

The Phase-0.5 mono-duplication trick (write `ch[0]` to both `ch[0]` and `ch[1]`) becomes degenerate for any real beamformer. Decision:

- **Primary fix (option B): `--bypass-beamformer` flag** on `ecnr_bench` and `ecnr_live`. Selects the two-arg `Init(rate, num_mics)` path → passthrough → `ch[0]` survives untouched. This is the path used for HAL-loopback smoke tests, AEC3 regression bisection, and CI runs that don't care about the beamformer.
- **Secondary fix (option A): multi-channel WAV ingestion** in `ecnr_bench` (only). When given a multi-channel input, bench passes channels through to `Frame::ch[c]` directly. Live stays single-physical-mic on macOS host (miniaudio); the bypass flag covers that case.
- **Rejected (option C): synthetic 1-sample shift** between `ch[0]` and `ch[1]`. Tempting because it's a one-line change, but it gives the beamformer fictitious geometry that doesn't match any real array, and it would let unit tests pass against a regression that real hardware would expose. Not worth the false signal.

Concretely:
- `ecnr_bench --bypass-beamformer` and `ecnr_live --bypass-beamformer` are Phase 1 deliverables, landing in the same change as the DSB implementation.
- Multi-channel WAV in `ecnr_bench` lands as a follow-up once we have a recorded multi-mic file from real hardware.

## Algorithms considered

| Algorithm | CPU (N=4, A55) | Robustness to geometry error | Convergence | Production precedent | Phase 1 verdict |
|---|---|---|---|---|---|
| **Delay-and-sum (DSB)** | Negligible (<<1% A55) | High — wrong steering = degraded SNR, never worse than ch[0] | Instant (no adaptation) | NXP VoiceSeeker fixed mode; athena-signal GSC's FBF stage; standard textbook | **Chosen for Phase 1** |
| **MVDR (Capon)** | Low-moderate (covariance + inversion per band) | Low — singular covariance on correlated noise / mismatched mics; can null target on steering error | Frames-to-seconds (covariance estimator) | athena-signal `dios_ssp_mvdr/`; classical wireless | **Phase 1.5 candidate** — needs cabin noise measurements first |
| **GSC** | Moderate (FBF + ABM + AIC, all adaptive) | Medium — Griffiths-Jim variants tolerate steering error better than raw MVDR | Seconds (adaptive blocking + AIC) | athena-signal `dios_ssp_gsc/`; production smart-speaker stacks; NXP VoiceSeeker | **Phase 1.5 candidate** — DSB ships as its FBF, so GSC is an additive upgrade |
| **LCMV** | Moderate (similar to MVDR + extra constraints) | Low — same singular-covariance issues as MVDR | Frames-to-seconds | Wireless / sonar, less common in voice | **Deferred** — not justified until passenger-zone isolation is a requirement |
| **Adaptive variants (NLMS-based, Kalman, neural)** | Variable, mostly higher | Mixed | Slow / data-hungry | Research; some smart-speaker neural front-ends | **Out of scope** for Phase 1 and Phase 1.5 |

DSB is the lower bound on what a "real" beamformer should do; MVDR/GSC are the upper bound we want to reach. The order — DSB first, GSC second — is the same order athena-signal builds its `gsc` kernel internally (FBF → ABM → AIC), so the Phase 1 work is reusable in Phase 1.5.

## Geometry options considered

| Option | Portability | Convergence | Implementation cost | Verdict |
|---|---|---|---|---|
| Hardcoded U300 geometry | None — tied to one hardware variant | Instant | Trivial | **Rejected** — violates the "one binary, 2–8 mics" goal from ADR-0004 |
| `MicGeometry` struct at `Init` | High — HAL/config supplies values | Instant | Small (struct + one overload) | **Chosen** |
| Runtime DOA estimation | Highest — geometry-agnostic | Frames-to-seconds | High (DOA kernel + tracker) | **Deferred to Phase 2+** — athena-signal has a `dios_ssp_doa` kernel we could lift if needed; not justified before MVDR/GSC are even in. |

NXP VoiceSeeker's "geometry-agnostic" framing ([Cellular Audio Processing Solutions Deep Dive.md:154](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md)) is implemented the chosen way: the integrator supplies coordinates at integration time, and the algorithm tolerates a wide range of array shapes and spacings. We adopt the same model.

## Trade-off analysis

The chosen pair (DSB + struct-at-`Init`) is the **median of the design space** on every axis: not the highest-quality algorithm, not the most ergonomic geometry source, but the one that lets us **ship a non-trivial beamformer in Phase 1 without coupling to unmeasured assumptions**. DSB needs only mic positions and a target direction; both are knowable from a cabin CAD drawing before any acoustic measurement is done. MVDR's noise-covariance estimator and GSC's adaptive blocking matrix both need real cabin recordings to tune — recordings we won't have until Phase 2 (cabin acoustics measurement).

The geometry struct is also the median: more flexible than hardcoding, less ambitious than DOA, and — critically — additive over the ADR-0004 interface, so Phase 0.5 callers don't have to be touched.

## Consequences

**What becomes easier:**
- Phase 1 has a concrete, low-risk first task: implement DSB against a fixed `MicGeometry`, ship under the existing `Beamformer` interface. Estimated 1–2 weeks of one engineer.
- The `--bypass-beamformer` flag gives bench/live a clean way to bisect "is this an AEC3 regression or a beamformer regression?" — without it, every regression is jointly attributable.
- DSB is the FBF in athena-signal's GSC, so Phase 1.5 (GSC) reuses the Phase 1 code as a subcomponent.
- HAL contract for geometry is explicit (`MicGeometry`) — U300 platform team gets a clear ask: "supply mic coordinates in meters at HAL init."

**What becomes harder:**
- Two `Init` overloads to maintain. Mitigated by having the two-arg form forward to the three-arg form internally.
- Bench/live now has a beamformer-bypass mode that needs to be exercised in CI, otherwise it bit-rots and the day we need it (during a regression hunt) it doesn't work.
- Once DSB ships, the bench/live mono-duplication trick stops being a no-op without the bypass flag — anyone who forgets the flag will see degraded output and not understand why. Mitigated by failing loudly: when the beamformer detects two channels with bit-identical content, log a one-shot warning.

**What we'll need to revisit:**
- All entries in **Open assumptions** below. Each is a candidate for a follow-up ADR or a Phase 2 measurement task.
- The `MicGeometry` struct shape, when LCMV / per-zone constraints become a requirement.
- The Phase 1.5 trigger (DSB → MVDR/GSC) once cabin acoustics are measured.

## Open assumptions

These are load-bearing assumptions that this ADR does not yet validate. Each is plausible but unmeasured.

- **U300 mic positions are knowable at HAL init.** The HAL needs to supply `MicGeometry::positions_m` from somewhere — vehicle config, OTP fuses, build-time constants, etc. If the HAL can't supply them, the bypass path is the fallback, and we ship without a real beamformer until that's resolved.
- **DSB's directional gain is enough for the cabin SNR target.** DSB gives 3 dB at N=2 and ~9 dB at N=8 against spatially-uncorrelated noise, but cabin noise (HVAC, road, wind) is partially-correlated and partially-directional. The true SNR uplift depends on the noise field, which we haven't measured. If DSB falls short, MVDR/GSC is the answer, not a different `MicGeometry`.
- **A single target direction is enough for Phase 1.** Driver-only voice capture; passenger-zone isolation is deferred. Confirmed feasible because the U300 product brief is voice-control + hands-free for the driver, with passenger isolation as a Phase 2+ feature.
- **Mic mismatch is small enough for DSB.** VoiceSeeker tolerates mismatched mics for DSB-class processing ([Cellular Audio Processing Solutions Deep Dive.md:154](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md)). Validate empirically once U300 mics are sampled; if mismatch is severe, add a per-mic gain calibration field to `MicGeometry`.
- **The `target_direction` default `(1, 0, 0)` is the right convention.** "Forward" should be the driver's mouth from the array origin. This is a documentation issue, not an algorithmic one; the HAL writes the right value or the bypass path applies.
- **AEC3 doesn't need to know about the beamformer.** AEC3 sees the post-DSB mono signal as if it were a single-mic capture. If DSB introduces a frequency-dependent group delay the render delay estimator misinterprets, we'll have a surprise. Mitigation: DSB at small mic spacings (2–8 cm per VoiceSeeker) introduces sub-millisecond inter-channel delay; well below AEC3's frame-level delay tracking resolution.

## Phase 1 entry criteria

This ADR's recommendations actually translate into code when **all** of the following are true:

1. Phase 1 has started (Phase 0.5 closeout signed off).
2. A55 cross-compile is green for the existing chain — no point landing a real beamformer until the binary builds for the target.
3. U300 mic count is locked (`num_mics ∈ [2, 8]`, specific value known).
4. Either (a) U300 mic positions are supplied via the HAL, or (b) a stand-in geometry (e.g., a typical 2-mic dashboard array) is documented as a placeholder for development.

Phase 1.5 (DSB → MVDR or GSC) entry criteria:

1. DSB has shipped and is exercised in bench/live on real multi-mic input.
2. Cabin noise field has been measured on at least one U300 vehicle, with recordings checked into a measurements repo.
3. A regression suite exists that:
   - Plane-wave from `target_direction` survives at unit gain (DSB sanity).
   - Plane-wave from 90° off `target_direction` is attenuated by the expected null pattern.
   - Singular-covariance input (two bit-identical channels) does not crash MVDR / does not produce NaN output.

## Action items

- [ ] **Phase 1, week 1:** add `src/pipeline/mic_geometry.h` per the struct definition above. No-op on the chain; just a header.
- [ ] **Phase 1, week 1:** extend `Beamformer::Init` with the three-arg overload; two-arg overload forwards to it with `kPassthroughGeometry`.
- [ ] **Phase 1, week 1–2:** implement fixed delay-and-sum in `src/pipeline/beamformer.cc`. Replace `ch[0]` selection with the DSB output; gate on the geometry being non-passthrough.
- [ ] **Phase 1, week 2:** unit tests:
  - Synthetic plane-wave from `target_direction` → SNR-preserving sum.
  - Synthetic plane-wave from 90° off → 6 dB+ attenuation at 1 kHz, 4 cm spacing, N=4.
  - Two-source anechoic mixture → in-direction source SNR improves; off-direction source attenuated.
  - `kPassthroughGeometry` → output bit-equal to `ch[0]`.
- [ ] **Phase 1, week 2:** add `--bypass-beamformer` flag to `ecnr_bench` and `ecnr_live`; default on for `ecnr_live` until U300 HAL supplies geometry.
- [ ] **Phase 1, week 3:** add multi-channel WAV ingestion to `ecnr_bench`.
- [ ] **Phase 1, week 3:** add a one-shot warning when two input channels are bit-identical and the beamformer is not in bypass mode (catches the Phase-0.5 mono-duplication trick continuing past its intended lifespan).
- [ ] **Phase 1 closeout:** measure DSB CPU on A55. Update this ADR with measured numbers, replacing the "<<1% A55" estimate.
- [ ] **Phase 2 (gated):** open **ADR-0011 — Adaptive beamforming (MVDR vs GSC)** once cabin recordings are in hand. Decision will be informed by athena-signal's [`dios_ssp_mvdr/`](../../vendor/athena-signal/athena_signal/kernels/dios_ssp_mvdr) and [`dios_ssp_gsc/`](../../vendor/athena-signal/athena_signal/kernels/dios_ssp_gsc) implementations as reference points.
- [ ] **HAL conversation, ASAP:** request U300 mic coordinates (in any consistent frame, in meters) from the platform team. Without these, Phase 1 ships in bypass mode by default.

## References

- [ADR-0001](0001-hybrid-aec-architecture-review.md) — open assumption A3 (multi-mic).
- [ADR-0004](0004-mic-array-2-to-8.md) — chain shape (per-mic `Frame::ch[]`, `Beamformer` upstream of AEC3).
- [ADR-0005](0005-render-tap-policy.md) — style precedent (decisive, code-pointer-aware, partly Phase-1-deferred).
- [`src/pipeline/beamformer.h`](../../src/pipeline/beamformer.h) — current header, with the `TODO(ADR-0010)` resolved by this ADR.
- [`docs/Cellular Audio Processing Solutions Deep Dive.md:154`](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — NXP VoiceSeeker geometry-agnostic spec (linear / triangular / circular, 2–8 cm spacing).
- [`docs/Cellular Audio Processing Solutions Deep Dive.md:175`](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — BdSound "Microphone Bubbles" passenger-zone isolation (Phase 2+ relevance).
- [`docs/Cellular Audio Processing Solutions Deep Dive.md:179`](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — VoiceSeeker resource scaling (14 MHz / 60 KB at N=2; 320 MHz / 290 KB at N=3 + AEC).
- [`vendor/athena-signal/athena_signal/kernels/dios_ssp_mvdr/`](../../vendor/athena-signal/athena_signal/kernels/dios_ssp_mvdr) — reference MVDR implementation (Phase 1.5 reading).
- [`vendor/athena-signal/athena_signal/kernels/dios_ssp_gsc/`](../../vendor/athena-signal/athena_signal/kernels/dios_ssp_gsc) — reference GSC implementation (FBF + ABM + AIC; Phase 1.5 reading).
- [`vendor/athena-signal/README.md`](../../vendor/athena-signal/README.md) — confirms MVDR/GSC require explicit `mic_coord` (mic_num × 3 array); same model adopted here as `MicGeometry::positions_m`.
- [`vendor/athena-signal/athena_signal/kernels/dios_ssp_doa/`](../../vendor/athena-signal/athena_signal/kernels/dios_ssp_doa) — DOA estimator, available if the Phase 2+ direction goes geometry-agnostic.

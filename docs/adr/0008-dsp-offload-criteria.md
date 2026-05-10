# ADR-0008: DSP offload trigger criteria — Phase 6 entry conditions

**Status:** Accepted
**Date:** 2026-05-10
**Supersedes action item 7 in:** ADR-0001 ("ADR-0008 — DSP offload decision criteria")

## Context

ADR-0001 commits the project to a hybrid pipeline (linear AEC backbone + neural post-processing) and to a Cortex-A55-only v1. Phase 6 of the [PROJECT.md](../../PROJECT.md) roadmap reserves the option to migrate the **linear AEC core** to a Tensilica HiFi / equivalent audio DSP — leaving the neural post-filter on A55 — but the decision itself is "deferred." That deferral is correct in 2026-05: we have no platform-locked DSP SKU, no cross-toolchain, no IPC story, and Phase 0.5 numbers (RTF 0.057 on macOS Apple Silicon) suggest the chain is comfortably realtime on a modern CPU.

The deferral becomes unsafe if it has no exit condition. "We'll think about Phase 6 later" is how projects accumulate hidden coupling that makes the move impossible by the time it's needed. The cost of *defining the trigger* is small; the benefit is that the team can keep building on A55 without the DSP option silently expiring.

This ADR fixes the criteria. Phase 6 itself stays deferred. The implementation, the SoC selection, the toolchain spike, and the IPC design are all out of scope here. What's in scope is **what observation forces us to start Phase 6.**

The source research ([Cellular Audio Processing Solutions Deep Dive.md](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md), [deep-research-report.md](../deep-research-report.md)) treats DSP offload as the standard production answer for sustained voice workloads (NXP SAF9100 + dual HiFi 5 + BdSound, MediaTek Genio + HiFi 4/5 + SOF, Retune VoiceSpot/VoiceSeeker on HiFi Mini). The Chinese report's framing — *"if the platform has a dedicated ADSP/NPU, push the heavy ECNR onto it"* — is the prevailing industry default, **not** the path we're on. We are on A55 because (a) v1 SoC is not committed to a HiFi-class DSP, (b) we want a debuggable single-core baseline before splitting across processors, and (c) the measurement says we don't need it yet.

## Decision

Phase 6 stays deferred. We adopt the following **trigger criteria**. They are evaluated against measured numbers from the Phase 1 A55 production binary and ongoing field telemetry, not against extrapolations.

### A. Trigger conditions (any one → spike)

A spike is a 1–2 person-week investigation: HiFi toolchain stand-up, IPC prototype, profiling target. It does not commit to shipping.

| # | Condition | Threshold | Measurement basis |
|---|---|---|---|
| **T1** | Sustained RTF on A55 production binary, mode = `AEC+NN`, typical workload | **RTF > 0.40** averaged over a rolling 60 s voice session at the *typical* DVFS operating point (not max f) | Phase 1/4 bench harness reports RTF per chain stage. T1 fires if the *combined* chain (Beamformer + AEC3 + neural RES + NS + AGC) exceeds 0.40 on the real target SoC. RTF 0.40 leaves <60% headroom for codec, RTP/JBM, AEC re-convergence bursts, and other A55 work — below industry comfort. |
| **T2** | Energy per second of valid speech (the metric that owns this project per [deep-research-report.md](../deep-research-report.md) line 9 and 165) | **> 35 mJ / s of valid speech**, or **> 25%** worse than the budget the U300 product owner ratifies | A55 board-level rail measurement, integrated over a representative call (mixed near-end-only, double-talk, far-end-only, idle), normalized to actual VAD-positive seconds. Mode controller already running. |
| **T3** | Thermal | Sustained voice workload causes A55 cluster to **enter DVFS thermal throttle** within **< 10 minutes** of continuous call at the U300 chassis ambient spec | In-chassis thermal log + governor events. Hitting this means realtime is no longer guaranteed across the full call duration. |
| **T4** | Frame-loop latency | 10 ms ProcessCapture wall-clock **p99 > 6 ms** sustained, or **any p100 > 9 ms** outside startup | The chain must finish a frame before the next arrives. p99 > 6 ms means we're one DVFS dip away from underruns; p100 > 9 ms means we already missed. |
| **T5** | Product-spec workload increase | A spec change adds workload that pushes A55-only over its share of the SoC compute budget — **e.g.** going from 2-mic to 6-mic beamforming, adding a second simultaneous voice zone, raising canonical rate from 16 kHz to 48 kHz fullband for the headline tier, or stacking a second always-on neural model (KWS, ASR frontend) on the same core | Quantitative — re-run T1/T2 against the new spec. Qualitative — a credible architect's review concludes the headroom evaporates. |
| **T6** | A55 SoC change | The chosen U300 SoC variant ships with a HiFi-class DSP **and** the BSP exposes a usable IPC channel (OpenAMP / SOF / vendor RPMsg) **and** vendor support for the toolchain is realistic — i.e. *the offload becomes nearly free* | Platform fact, not a measurement. If the DSP is sitting there idle and the integration cost drops by an order of magnitude, the calculus inverts. |

### B. Commit condition (any TWO of T1–T5 simultaneously, or T6 alone → commit)

A "commit" means Phase 6 enters the active roadmap with budget and milestones. Until then the spike output stays in the drawer.

The asymmetry between spike (one fires) and commit (two fire, or T6) is deliberate. Single failures are usually fixable on A55 first — a NEON kernel re-write, a quantization upgrade, a mode-controller policy tweak, a thread-affinity fix. Two failing simultaneously means the **single-core architecture itself** is the wall, and we need the DSP. T6 short-circuits this because the cost calculus has flipped without us doing anything.

### C. Pre-trigger A55-side mitigations (must be exhausted before commit)

Before declaring T1/T2/T3/T4 fatal we **must** have:

1. NEON-vectorized FFT / FIR / complex MAC kernels on the linear AEC hot path (per [deep-research-report.md](../deep-research-report.md) §"Cortex-A55 落地" — vectorization is the first lever, not DSP offload).
2. INT8 / quantized neural RES post-filter via XNNPACK or equivalent NEON-optimized runtime (ADR-0007 territory).
3. Mode-controller-driven depth switching active, with verified DVFS-friendly batching.
4. Single-core affinity + pinned scheduler priority on the audio thread.

If those are not in place, the trigger is "we have not finished optimizing A55," not "A55 is insufficient."

## What moves to DSP

Components that are **frequency-domain block math, deterministic, with bounded state**:

| Component | Move? | Why |
|---|---|---|
| Linear AEC adaptive filter (AEC3 NLMS / RLS / Kalman update) | **Yes** | The classical hot path. Decades of HiFi DSP precedent. Frequency-domain block + FIR taps + adaptive update is exactly what the Tensilica VLIW / SIMD pipeline is built for. |
| FFT / iFFT for the AEC analysis/synthesis | **Yes** | Vendor-tuned FFT in HiFi NatureDSP / Audio Weaver IP outperforms hand-rolled NEON by a wide margin. |
| Render reference ring buffer + delay tracker | **Yes** | Lives next to the AEC; cheap to keep on the same processor. |
| Resampler (Speex / vendor) | **Maybe** | Move with AEC if the sample-rate conversion sits between mic ingest and AEC; otherwise leave it on A55 to avoid extra IPC hops. |
| Beamformer (delay-and-sum / MVDR / GSC, when ADR-0010 lands) | **Yes** | Same shape as AEC: block, frequency-domain, deterministic. Wants to live next to the AEC for buffer locality. |
| Classical NS (Wiener / MMSE-LSA) — *the low-power tier only* | **Yes** if used | Same reasoning. Not relevant if RNNoise replaces it. |

## What stays on A55

Components that are **neural, dynamically-shaped, or control-plane**:

| Component | Stay? | Why |
|---|---|---|
| Neural RES post-filter (NKF-AEC / DTLN-AEC / similar) | **Yes — A55** | DSPs are hostile to dynamic neural graphs. Even on HiFi iQ, where AI MAC density is significant, deploying a custom small-RNN with arbitrary topology and the ability to update the model out-of-band is materially harder than running it via XNNPACK on A55. The model also potentially shares weights or runtime with other A55 NN workloads (KWS, ASR frontend); fragmenting NN execution across processors costs more than it saves. |
| RNNoise NS post-filter | **Yes — A55** | Same reasoning. Small enough that the IPC round-trip would dominate. |
| AGC | **Yes — A55** | Decision logic, not signal-processing throughput. Cheap on A55. |
| Mode controller (idle / NS-lite / AEC / AEC+NN selection) | **Yes — A55** | Pure control plane. Has to live where the application policy lives. |
| `AecChain` orchestration, frame-shape glue, HAL interface | **Yes — A55** | This is the boundary; keeping it on A55 keeps the `AecChain` interface unchanged regardless of whether AEC3 runs locally or via IPC. |
| ERLE / RES likelihood / level-estimator stats reporting | **Yes — A55** | Aggregation of per-frame stats; not on the hot path. |

This split is deliberately the same one ADR-0001 committed to in principle ("path to DSP offload — clean: linear AEC moves to DSP, neural stays on ARM"). This ADR does not invent it; it just promises to honor it when the trigger fires.

## Architectural split

```
                            A55 (Linux/RTOS host)                                │  HiFi DSP (RTOS)
                                                                                 │
   mic[N]  ──► [HAL ingest]                                                      │
                    │                                                            │
                    │ (zero-copy shared-mem ring, frame-aligned)                 │
                    ├─────────────────────────────────────────────────────────►  │ ──► [Beamformer]
                    │                                                            │       │
                    │                                                            │       ▼
                    │                                                            │     [Linear AEC: AEC3 core]
                    │                                                            │       │
                    │                                                            │       ▼
                    │  ◄─────────────────────────────────────────────────────────│ ──── (AEC residual + linear-stage stats)
                    ▼                                                            │
            [neural RES]  ──►  [NS]  ──►  [AGC]  ──► uplink                     │
                                                                                 │
            [mode controller]                                                    │     [delay tracker]
                    │                                                            │       ▲
                    │ (control msgs: mode, gain, bypass — low-rate)              │       │
                    ├─────────────────────────────────────────────────────────►  │ ──────┘
                                                                                 │
   render-tap ──► [HAL] ──► (zero-copy shared-mem ring) ────────────────────────►│ (reference, post-DRC/EQ per ADR-0005)
```

**IPC channel.** Zero-copy shared memory ring for audio buffers (mic in, residual out, reference in). Out-of-band low-rate message channel (RPMsg / OpenAMP / SOF mailbox depending on platform) for mode, gain, bypass, and stats. The Cellular Audio Processing report ([line 76](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md)) flags zero-copy as the production-quality answer for AP↔DSP audio: copy-based IPC eats the latency budget.

**Frame alignment.** 10 ms canonical frame stays canonical across the boundary. The DSP processes 10 ms blocks; the A55 hands it 10 ms blocks. No re-blocking.

**Latency budget.** The 10 ms frame loop must absorb: A55→DSP enqueue + DSP processing + DSP→A55 dequeue + neural post + NS + AGC. We budget **≤ 4 ms total IPC overhead** (enqueue + dequeue, both directions, with cache coherence flushes). The DSP processing itself should run faster than realtime on the linear stage by a comfortable margin (≤ 2 ms for the AEC core); the neural post on A55 fits in the remaining time. If the IPC overhead measurement at spike time is > 4 ms, that is itself a Phase 6 blocker and a reason to walk back to A55-only.

**Debuggability.** Cross-processor debugging is materially harder. The `AecChain` interface (ADR-0006) hides the boundary so unit tests can replace the DSP backend with an in-process `Aec3Adapter`. CI runs the in-process variant; on-target tests run the IPC variant. Both must produce bit-equivalent or near-equivalent output for the same input — divergence is a bug, not a feature.

## Options considered

### Option A — Defer indefinitely (status before this ADR)

| Dimension | Assessment |
|---|---|
| Engineering cost | Zero now |
| Risk that it happens accidentally | High — without exit conditions, "deferred" silently becomes "abandoned" or "panic later" |
| Risk that we couple the code in ways that block it | Medium — if no one is watching for the boundary |

**Rejected.** Not because deferral is wrong — it is correct *today* — but because deferral without criteria is unfalsifiable.

### Option B — Commit to Phase 6 from day one

| Dimension | Assessment |
|---|---|
| Engineering cost | High — HiFi toolchain learning, NDA, Xtensa C/C++ quirks, separate test harness, more complex CI, A55↔DSP IPC code |
| Engineering risk | High — we don't know the U300 SoC DSP variant, BSP support, vendor toolchain availability |
| Runtime benefit | Theoretical — Phase 0.5 RTF is 0.057 on macOS; A55 estimate 0.15–0.25 — far below any threshold that would justify the cost |
| Reversibility | Low — once shipped with a DSP partition, walking back is rework |

**Rejected.** Premature. The measurement does not yet say A55 is insufficient, and the SoC fact does not yet say the DSP is free.

### Option C — Gated by criteria (chosen)

| Dimension | Assessment |
|---|---|
| Engineering cost now | Small — write the criteria (this ADR), keep `AecChain` interface boundary clean (already done in ADR-0006) |
| Engineering cost later | Bounded — spike on trigger, commit on two triggers, ship if commit |
| Runtime benefit | Realized exactly when needed |
| Reversibility | High while in A55-only mode; medium once shipped with DSP — the `AecChain` interface keeps the boundary swappable, but a shipped product with DSP firmware accumulates calibration and tuning that's painful to throw away |

**Chosen.** Same shape as ADR-0005's "decide the policy now, defer the implementation": the *criteria* are decided today; the *trigger* may or may not fire later.

## Cost / benefit analysis

### Engineering cost (if Phase 6 commits)

| Item | Estimate | Notes |
|---|---|---|
| HiFi toolchain stand-up (Xtensa C/C++, build, on-target debug, simulator) | 4–6 person-weeks | Cadence NDA likely required for full Xtensa Xplorer; vendor BSP varies |
| AEC3 port to HiFi (or replacement with HiFi-native frequency-domain AEC) | 6–10 person-weeks | AEC3 is C++ with WebRTC-isms; full port is non-trivial. Alternative: Audio Weaver IP block, NXP SAF-style BdSound integration, or a hand-written block AEC. ADR-0001 line 124 ("frequency-domain block AEC with explicit longer-tail support") becomes cheaper if we go this route |
| IPC infrastructure (shared memory, RPMsg, frame-aligned ring) | 3–4 person-weeks | OpenAMP / SOF reduces this if the BSP supports it |
| Test harness on DSP, dual-target CI | 2–3 person-weeks | Has to run on host (in-process), on A55 (in-process), and on A55+DSP (IPC). Bit-equivalence target |
| Cross-processor debug + integration | continuous | Real cost is the long tail |
| **Total spike** | **~1–2 person-weeks** | Just to confirm feasibility on the chosen platform |
| **Total commit (port + ship)** | **~4–6 person-months** | One engineer focused, plus reviewers |

### Engineering risk

- HiFi SKU on U300 is not known at ADR time (see Open assumptions). Picking the wrong target wastes weeks.
- Cadence vendor support tier — if the project doesn't have a Cadence FAE relationship, the toolchain learning curve is steep.
- BSP IPC primitives — if the platform doesn't expose zero-copy shared memory between AP and DSP, the latency budget breaks (research line 76).
- Two-target CI complexity. Bit-equivalence assertions are flaky in practice when vendor FFT differs from WebRTC's own FFT; fuzzy tolerance is needed and that hides real bugs.

### Runtime benefit (rough order)

- Linear AEC on dedicated DSP at 200–800 MHz HiFi is typically **5–15× lower mW per MIPS** than A55 doing the same work at typical DVFS — research line 159 (NXP VoiceSeeker AEC on Cortex-M7 at 320 MHz consumes the entire AEC + beamforming + 3-mic chain). The A55 frees up cycles for the neural post-filter, the NS, and other concurrent SoC work.
- RTF reduction on the A55 chain: removing AEC3 and FFT from A55 takes the dominant share of the linear-stage cost out (Phase 0.5 measurement: AEC3 is the largest single contributor). Estimated chain RTF on A55 with linear stage offloaded: **0.3–0.5×** the A55-only number.
- Power reduction: order of **20–35%** energy-per-second-of-valid-speech, primarily by letting the A55 cluster idle / DVFS down between neural post bursts while the DSP runs continuously at low frequency.
- These numbers are estimates from the research order-of-magnitude — they must be confirmed at spike time before the commit gate.

## Reversibility

The `AecChain` interface (ADR-0006) is the load-bearing reversibility lever. As long as:

1. The vendor types stay opaque-pimpl (already enforced).
2. The AEC backend is selected at construction, not compiled in.
3. The IPC variant (`Aec3DspAdapter` or similar) implements the same interface as the in-process variant (`Aec3Adapter`).

…then a product can ship with the DSP path and a later product variant can ship without it without reworking the chain. Calibration data (delay, ERLE baselines, tuning parameters) does carry forward.

What is **not** reversible without rework:
- DSP firmware update path. Once the production fleet has DSP firmware that owns the AEC, removing it from new SKUs means the new SKUs are different products and need their own bring-up.
- Vendor IP licensing. If we ship Audio Weaver / BdSound / Retune-licensed code on the DSP, the license obligations persist.

The ADR therefore does not claim "we can flip back to A55-only after launch." It claims the *codebase* stays flippable; the *product* commits when it ships.

## Open assumptions

### O1. The "C1 / BX2" pair in PROJECT.md and ADR-0001 is shorthand, not a literal SKU pair

PROJECT.md and ADR-0001 use "Tensilica HiFi C1 / BX2" as the label for the offload target. The source research describes:
- **Cadence Tensilica HiFi family**: HiFi Mini, HiFi 3z, HiFi 4, HiFi 5, HiFi 5s, HiFi iQ — generations, not "C1."
- **CEVA SensPro2 / BX2** — a CEVA core (BX2 is a CEVA-IP designation), distinct from the Cadence Tensilica family.

The "C1 / BX2" pairing therefore conflates a Cadence core (likely intended as "HiFi 1" or an internal short name) with a CEVA core. The criteria in this ADR are SoC-vendor-agnostic; they fire the same way whether the eventual offload target is a HiFi 4, HiFi 5, HiFi iQ, CEVA SensPro2, or something else. The naming is to be cleaned up when O2 resolves.

### O2. The U300 SoC variant has not been chosen / disclosed

We do not yet know whether the U300 silicon has a HiFi-class DSP, a CEVA core, an NXP-style dedicated audio DSP (SAF9100-shape), or just the A55 cluster. This is the biggest unknown for T6 and for the implementation cost estimate. **Action item:** identify HW/platform contact, confirm SoC family, document DSP availability (or lack of it).

### O3. Vendor toolchain access is unknown

Cadence Xtensa Xplorer requires NDA and licensing. CEVA's toolchain similarly. Without confirmed access, the spike cost estimate is a lower bound. **Action item:** identify Cadence / CEVA contact concurrently with O2.

### O4. DSP firmware update path on production hardware

Even if Phase 6 ships, fielded units must have a path to update DSP firmware (for security patches, AEC tuning updates, model updates if the post-filter ever moves). Some automotive platforms gate this through the SoC vendor with multi-month release cycles. If the U300 platform doesn't support host-driven DSP firmware updates, that materially constrains what we can ship on the DSP — favoring frozen, well-tested classical AEC over algorithm experimentation.

### O5. Bit-equivalence between A55 in-process AEC and DSP IPC AEC

Vendor FFT libraries diverge from WebRTC's own FFT in the last bit (rounding, denormal handling). CI bit-equivalence assertions become fuzzy-tolerance assertions, which hide some classes of bug. Open question for spike time: is fuzzy bit-equivalence acceptable, or does the IPC variant need its own dedicated test corpus?

### O6. Order-of-magnitude benefit estimates need spike-time confirmation

The "5–15× mW/MIPS reduction," "0.3–0.5× chain RTF on A55," and "20–35% energy-per-second" estimates above are extrapolations from the research, not measured numbers on our specific platform. They are sufficient for "whether it's worth a spike" but not for "whether to commit." The spike output must include real measurements that replace these estimates before the commit gate is evaluated.

## Action items

| # | Action | Phase | Owner |
|---|---|---|---|
| 1 | Phase 1: implement A55 production binary RTF + power benchmark harness emitting T1–T4 metrics on every CI run | Phase 1 | Bench owner |
| 2 | Phase 1: define the *typical DVFS operating point* for U300 explicitly — frequency, voltage rail, governor mode used to evaluate T1 | Phase 1 | Platform |
| 3 | Phase 1: define the U300 product power budget for sustained voice → fixes the absolute number behind T2 | Phase 1 | Product |
| 4 | Phase 1: integrate thermal log scraping into the bench harness → T3 | Phase 1 | Bench owner |
| 5 | Phase 4: integrate field-telemetry RTF / latency / thermal counters into the production binary so triggers are measurable post-deployment, not just on the bench | Phase 4 | Telemetry |
| 6 | Identify HW / platform contact for U300; confirm SoC family + DSP availability (resolves O2) | Now (no later than Phase 1) | Project lead |
| 7 | Identify Cadence and/or CEVA contact for toolchain availability + NDA path (resolves O3) | Concurrent with #6 | Project lead |
| 8 | Phase 4 review: re-evaluate this ADR's thresholds against the production-A55 numbers. If T1/T2 budgets are clearly wrong (off by >2× either way), update them in a follow-up ADR | Phase 4 review | Project lead |
| 9 | Clean up the "C1 / BX2" naming in PROJECT.md and ADR-0001 once O2 resolves the actual target | After O2 | Doc owner |

## References

- [PROJECT.md](../../PROJECT.md) — Roadmap Phase 6 row; Architecture section
- [ADR-0001 — Hybrid linear-AEC + neural-post architecture](0001-hybrid-aec-architecture-review.md) — Action item 7 (this ADR); Option A "path to DSP offload" row; line 56 Phase 6 row of the roadmap
- [ADR-0005 — Render-tap policy](0005-render-tap-policy.md) — Tone and shape precedent; the render-tap location is *the* invariant the DSP path must respect
- [ADR-0006 — AecChain interface alignment with WebRTC APM](0006-aecchain-webrtc-alignment.md) — Opaque pimpl boundary that makes the DSP swap reversible
- [docs/Cellular Audio Processing Solutions Deep Dive.md §"Deep Dive: Cadence Tensilica HiFi DSP Solutions"](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — Cadence HiFi family description; line 80 SOF / Audio Weaver; line 76 OpenAMP / zero-copy IPC; line 169 SAF9100 / BdSound
- [docs/Cellular Audio Processing Solutions Deep Dive.md §"Power Management Constraints in Cellular IoT Implementations"](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — Cascaded activation states; mW-scale wake-word on HiFi Mini; energy gating
- [docs/deep-research-report.md §"Cortex-A55 落地与评测方法"](../deep-research-report.md) — "Energy per second of valid speech" metric (T2); NEON-first optimization order (T1 mitigations); DVFS-friendly batching; mode controller pattern
- [docs/deep-research-report.md vendor-IP table](../deep-research-report.md) — "若芯片带 HiFi DSP，ECNR 最好 offload" — the prevailing industry default this ADR gates against

# Design: GB/T 45314-anchored test corpus + Cortex-A55 memory/CPU optimization loop

**Date:** 2026-07-04
**Status:** Approved for implementation (autonomous session; decision points resolved from research + project precedent, recorded here)
**Drives:** user request — "review current implementation and test harness; find a public-dataset test solution linked to a standard requirement (ideally a Chinese automotive standard); develop the solution with the best possible memory footprint and computation requirement on the ARM Cortex-A55; self-loop test → improve."

## 1. Current-state review (2026-07-04, main @ 2be907c)

**Chain topology** (`src/pipeline/`): Beamformer (DSB / passthrough) → WebRTC AEC3 + HPF (APM instance #1) → RNNoise NS with VAD-gated wet/dry blend (16 k tier resamples 16→48→16 kHz via SpeexDSP q5) → WebRTC AGC2 adaptive-digital (APM instance **#2**, post-NS per ADR-0001; on by default since v0.4.1).

**Test harness**: 30-clip Microsoft AEC-Challenge real-recordings subset (SHA-pinned manifest), scored DNSMOS P.835 + scenario-aware AECMOS, enforced against ADR-0012 §2.1 v2 floors + §3.1 applicability matrix by `reference/run_aec_challenge.py`; CI-enforced on push/PR. Plus `ecnr_eval` ERLE harness (ADR-0011) and unit tests.

**Baseline measured this session** (host Apple Silicon, RelWithDebInfo, AGC on):

| Metric | Value |
|---|---|
| AEC-Challenge corpus verdict | **PASS** |
| cpu_ms_per_frame (30-clip p50 / max) | 0.205 / 0.229 ms |
| Host RTF | 0.020 |
| Peak RSS / peak footprint (single clip) | 14.6 MB / 4.5 MB |
| Host binary (RelWithDebInfo, unstripped) | 4.76 MB |
| aarch64 binary (Release+strip+int8, from perf spike) | 4.25 MB, qemu RTF 0.81 |

**A55 projection**: in-order 2-wide A55 @ ~1.8 GHz is realistically 10–15× slower than this host per-thread → **~2–3 ms per 10 ms frame ≈ 20–30 % of one core**. Feasible today, but headroom matters: Phase 3 wants a neural RES budget on the same core.

**Review findings (perf-relevant, ranked):**

1. **Two full APM instances per frame.** `Agc2Adapter` builds its own `webrtc::AudioProcessing` with everything but AGC2 disabled. Every capture frame pays a second APM `ProcessStream` (buffer marshalling, level analysis, framework overhead) plus a second APM's heap. Original PROJECT.md intent was AGC2 as a flag on the existing APM; the split preserves post-NS ordering (ADR-0001). Candidate: measure the split's true cost first; only merge if the quality gate proves order-insensitivity — otherwise keep.
2. **`Frame::ch{}` zero-initializes 7.68 KB per construction.** `ProcessCapture` constructs two Frames per 10 ms frame (`post_bf`, `post_aec`) → ~15 KB of memset per frame; only 320 B of each is live at 16 k mono. The header already documents the tail as "uninitialized scratch" — the `{}` contradicts the contract's intent.
3. **16 k tier NS path**: SpeexDSP q5 up + RNNoise 480-sample fullband frame + SpeexDSP q5 down + 4 conversion/copy loops. RNNoise is 48 k-native so the resample is structural, but q5 and the copies are tunable.
4. **RNNoise int8 path (Move B) already merged** to main (`DISABLE_DEBUG_FLOAT` + fetch-vendor sed patch, commit 5a5db1f). The cross-build spike tuned for cortex-a57 (ARMv8.0) — **DotProd (SDOT) is left on the table**: Cortex-A55 implements ARMv8.2 dot-product instructions, and RNNoise's `vec_neon.h` has an `__ARM_FEATURE_DOTPROD` path that the int8 GEMV would use. Candidate: `-mcpu=cortex-a55` on the cross-build.
5. **webrtc-apm builds `--buildtype=release` with NEON disabled on macOS hosts** (enabled on aarch64 cross). Host numbers therefore *underestimate* APM speed on target — acceptable for relative A/B, noted for honesty.
6. Bench-only overheads (mono→2ch duplication + DSB) are excluded from gate numbers via `--bypass-beamformer` — already correct.

**Unmerged prior work discovered:** `origin/perf/a55-baseline-and-size` is fully merged into main except one CMakeLists comment line + PROJECT.md notes. `codex/gb45314-release-gate` (unmerged) carries a complete **GB/T 45314-2025 eCall pre-compliance gate**: ADR-0013, five deterministic test conditions, clause-mapped floors (TCL≥46 dB proxy, convergence profile, ≤6 dB path-change swing, double-talk near-end preservation, B2 noise stability), stage-tap diagnostics (`post_bf/post_aec/post_ns/post_agc` WAVs), and an extended `ecnr_eval --run` CSV schema. Its 2026-05-27 verdict: B2 fixable by tuning, **double-talk blocked** — measured *before* the AGC-on default and v0.4.x tuning landed.

## 2. Test-corpus decision (standards linkage)

**Requirement**: public data, reproducible fetch, linked to a standard a Chinese automotive customer recognizes.

**The anchor standard is GB/T 45314-2025** 《道路车辆 免提通话和语音交互性能要求及试验方法》 (hands-free + voice-interaction performance for road vehicles), the speech-quality reference invoked by China's mandatory eCall regulation **GB 45672-2025** (implementation 2027-07-01). This supersedes generic ITU-T P.1100-series anchoring for this product: U300 is a China-market automotive terminal.

**Approaches considered:**

- **A. Keep the codex branch's synthetic conditions as-is.** Deterministic, zero download, already clause-mapped. But "synthetic road-noise proxy" is exactly what was rejected in the 2026-05-31 session ("not properly done") — real recordings are the project's bar since the AEC-Challenge integration.
- **B (chosen). Port the GB/T 45314 gate intact, then upgrade its noise-bearing conditions (B1 road, B2 HVAC) to real public recordings** fetched + SHA-pinned exactly like `datasets/aec_challenge/` (research agent verifying: ETSI EG 202 396-1 binaural car-noise database as first choice — it is the noise-reproduction basis the hands-free test standards family builds on — with DEMAND `TCAR` (Zenodo, CC) as fallback). The clause mapping, floors, and checker stay; only the stimulus source improves. Convergence/path-change/double-talk conditions stay deterministic-synthetic *by design* — they probe timing behaviour, not noise realism, and GB/T 45314 defines them as controlled signals, not recordings.
- **C. Build a fresh corpus ignoring the codex branch.** Discards working clause-mapped tooling for no gain. Rejected.

**Role separation** (unchanged): the AEC-Challenge corpus remains the *perceptual* enforced CI gate (ADR-0012). The GB/T 45314 gate is the *China pre-compliance* checkpoint — objective ERLE/level/stability proxies per clause. The perf loop must keep the first green and track the second.

## 3. A55 optimization loop (the "/goal")

**Metrics per iteration** (all scripted, no manual steps):
- Quality: `run_aec_challenge.py` corpus verdict + per-scenario p50s (must stay PASS; deltas logged), unit tests green, GB/T 45314 gate verdict tracked (informational until its own blockers clear).
- Compute: corpus `cpu_ms_per_frame` p50/max (host Release), qemu-aarch64 RTF via `scripts/cross-build-yocto/build.sh --smoke` for ARM-side confirmation of major moves.
- Memory: `/usr/bin/time -l` max RSS + peak footprint on a fixed clip; binary size (host Release stripped + aarch64).

**Loop protocol**: one move per iteration → build → gate → measure → keep (commit with numbers in message) or revert (record why in the perf log). Move ordering is measurement-driven: instrument first, optimize where the time actually is.

**Move backlog (initial ranking):**
- M0 — per-stage timing instrumentation in `AecChain` (aec3/ns/agc split; render vs capture) behind a bench flag. Decides M1/M3 priority.
- M1 — remove `Frame::ch{}` zero-init (contract already says scratch is uninitialized); measure.
- M2 — SpeexDSP resampler quality 5→3 A/B (NS path); quality gate arbitrates.
- M3 — single-APM experiment (AGC2 into APM #1, order becomes pre-NS): only if M0 shows the second APM is a material cost AND the corpus gate stays PASS with non-degraded p50s. Otherwise closed as "measured, kept split".
- M4 — cross-build `-mcpu=cortex-a55` (DotProd for int8 GEMV) + qemu A/B.
- M5 — binary trims: strip host Release, `-Os` on non-hot libs, dead ONNX-era code, LTO experiment.
- M6 — copy elimination in adapters (uplink copy loop, input_saved memcpy) if M0 shows them.

**Non-goals**: absolute A55 timings (no U300 SDK/hardware yet — qemu is relative-only), formal GB/T 45314 lab items (HATS/POI/P.863 — listed in ADR-0013), Phase-3 neural RES (separate work item; this loop *creates* its CPU budget).

## 4. Deliverables

1. GB/T 45314 gate ported to main (ADR-0013 + scripts + eval extensions + stage taps), current-chain verdict re-measured with AGC-on defaults.
2. `datasets/vehicle_noise/` (or equivalent) real-noise fetch + manifest, wired into the gate's B1/B2 conditions — pending research-verified URLs; if no public source survives verification, the synthetic conditions stay with an explicit "synthetic proxy" label and the fetch design is recorded for when access clears.
3. Perf loop executed ≥3 moves with a written iteration log (`docs/perf/a55-optimization-log.md`), REPORT.md + PROJECT.md refreshed.
4. Updated stale repo docs (CLAUDE.md / AGENTS.md still claim "documentation-only repository").

## 5. Risks

- GB gate port conflicts with post-branch `aec_chain`/`eval_main` changes → resolve at port time; the additions are mostly parallel (+367 lines eval, +11 chain).
- ETSI/ITU download terms or URLs fail verification → fallback chain (DEMAND TCAR) or keep synthetic with label; do not ship an unverifiable fetch script.
- Single-APM merge changes AGC behaviour (pre-NS level estimation) → gated by corpus PASS + explicit ADR note if adopted; default is to keep the split.
- qemu RTF ≠ A55 RTF (documented 5–10× emulation overhead) — used for proportional A/B only, never absolute claims.

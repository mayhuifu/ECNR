# ADR-0007: Neural runtime — TFLite + XNNPACK as the default; raw weights for NKF-AEC; ONNX RT only as a Phase-3 fallback

**Status:** Accepted (provisional — Phase 3 spike will confirm INT8 viability and binary-budget ceiling; see open assumptions)
**Date:** 2026-05-10
**Deciders:** Project lead
**Supersedes ADR-0001 action item:** #6 ("ADR-0007 — Neural runtime")

## Context

ADR-0001 fixes the architecture as `linear AEC (WebRTC AEC3) → neural RES → NS (RNNoise) → AGC`. The neural component is a *post-filter*, not the main echo cancellation. The candidate models surveyed in the research are heterogeneous in both size and format heritage:

| Candidate | Params | Compute | Native format (per vendored upstream) | Sample rate |
|---|---:|---|---|---:|
| **NKF-AEC** (Tencent, BSD-3) | 5,300 | RTF 0.09 (paper) | PyTorch `.pt` (`vendor/nkf-aec/src/nkf_epoch70.pt`, 28 KB); no TFLite, no ONNX | 16 kHz (n_fft=1024, hop=256) |
| **DTLN-AEC** (Westhausen) | <1 M | <1 M MAC/frame | TFLite (`vendor/dtln-aec/pretrained_models/dtln_aec_{128,256,512}_{1,2}.tflite`, 2.0–24.2 MB) | 16 kHz |
| **DeepFilterNet** (Schroeter, MIT/Apache-2.0) | ~2 M | low GFLOP | PyTorch `.zip` + ONNX `.tar.gz` (`vendor/deepfilternet/models/DeepFilterNet{2,3}_onnx.tar.gz`, ~8 MB) | **48 kHz native** |
| **GTCRN** (reserved fallback) | 48.2 K | 33.0 MMAC/s | research code | 16 kHz |
| **U-Net RES** (reserved fallback) | 136 K | 1.6 GFLOP/s, 10 MB | research code | 16 kHz |

The runtime decision is deliberately scoped to the **first three candidates**. GTCRN and U-Net RES are kept as bench-fallback options; their runtime falls out of the same recommendation once they get exported.

The hardware target is **Cortex-A55 with NEON**, no NPU, plus the macOS Apple-Silicon dev host and the Phase-1 Linux/aarch64 cross-compile target. ADR-0003 locks 16 kHz baseline + 48 kHz fullband, both at 10 ms frames; ADR-0006 fixes the `AecChain` interface that the neural stage will eventually plug after.

The decision must answer four questions, in order:

1. Which inference runtime do we standardise on for the chain?
2. Which quantisation strategy (INT8 / FP16 / FP32) for production vs. development?
3. What does the per-model recommendation look like, given the empirical formats above?
4. What is the binary-footprint budget for `libecnr_pipeline` once the neural stage lands?

## Decision

### 1. Runtime: TFLite (LiteRT) C++ with the XNNPACK delegate

**Standardise on TFLite (LiteRT) C++ + XNNPACK** as the default runtime for any neural component shipped in `libecnr_pipeline`. Pinned version: **TFLite 2.16.x** (matches the LiteRT split; pre-built static libs available for macOS-arm64 and linux-aarch64). XNNPACK delegate is enabled at `Interpreter` build time — it owns the NEON-optimised kernels for FC / depthwise / 1D-conv / GRU-unroll, which is exactly the operator footprint of DTLN-AEC and the U-Net RES fallback. Fallback to the TFLite reference kernel where XNNPACK does not cover an op, with a one-time log warning so we notice op-coverage holes.

The runtime lives behind a project-internal `ecnr::nn::Runtime` interface (sketch below). No `tflite::` symbols leak past that header — same pattern as `webrtc::AudioProcessing` is hidden behind `AecChain::Impl` in ADR-0006. This keeps a future swap to ONNX RT or a custom kernel a *file change*, not an architecture change.

### 2. Quantisation plan

| Build | Quantisation | Reason |
|---|---|---|
| **Dev / host** (macOS-arm64, linux-x86_64 CI) | **FP32** | Bit-exact comparison against the reference Python / Rust upstream during model porting and bench replay. Removes "is the bug in the model or in the quantisation?" as a confound. |
| **Phase-1 cross-compile target** (linux-aarch64) | **FP32 first**, then **FP16** once XNNPACK FP16 path is verified on A55 | A55's NEON FP16 fused-MAC (FEAT_FP16) gives ~2× throughput vs. FP32 with negligible accuracy loss for these models — published TFLite/XNNPACK guidance for A55/A75. FP16 is the **expected default** for Phase-3 first integration. |
| **Production** | **INT8 (post-training, full-integer)** for DTLN-AEC and DeepFilterNet *only after* a Phase-3 accuracy spike confirms the residual-echo metrics survive (open assumption O1) | INT8 ~4× model-size reduction and 1.5–2× speed-up over FP16 on A55. The spike measures ERLE / `residual_echo_likelihood` / PESQ delta vs. FP32. If the delta is >0.5 dB ERLE or >0.05 PESQ at the AEC working point, **stay on FP16** for that model. AEC residuals are small-magnitude signals; INT8 quantisation noise can be of the same order, so this is a real risk, not theoretical. |

**No QAT (quantisation-aware training)** in the project for v1. We don't own the training pipelines for these upstream models; QAT requires touching training code, dataset, and infra. Phase 3 ships PTQ; QAT is an option for Phase 4+ if PTQ accuracy is insufficient.

### 3. Per-model recommendation

| Model | Runtime | Quant (prod) | Footprint estimate (model only) | Why |
|---|---|---|---:|---|
| **NKF-AEC** | **Raw weights, hand-rolled inference** (no TFLite) | FP32 → FP16 | ~10–30 KB FP32 / ~5–15 KB FP16 / ~3–8 KB INT8 | 5.3 K params is **two orders of magnitude smaller** than TFLite's runtime overhead. The model is a single complex GRU + two complex linear layers + an outer Kalman-gain loop (see `vendor/nkf-aec/src/nkf.py:62-92`). The complex-valued GRU ("ComplexGRU" — four real GRUs cross-mixed) and the per-frame Kalman update are not ops TFLite Converter handles cleanly: PyTorch → ONNX → TFLite would expand the complex multiplies into a graph of real ops, lose the loop structure, and force per-frame interpreter invocation. The hand-rolled version is ~300 lines of NEON-friendly C++ against `Eigen` (already a transitive dep of WebRTC) and bit-exactly mirrors the PyTorch reference. License: BSD-3 (confirmed in `nkf.py` header). |
| **DTLN-AEC** | **TFLite + XNNPACK** | FP16 default, INT8 if O1 passes | 2.0–24.2 MB depending on tier | TFLite is the **upstream-shipped format**: `dtln_aec_{128,256,512}_{1,2}.tflite` is what the authors give us. Phase-3 starts from `dtln_aec_128_1.tflite` (1.99 MB) — the smallest of the six, and the most A55-realistic tier. The 256/512 variants are kept available for the fullband (48 kHz) tier or for higher-quality desktop bench, but are not the production target. License: open assumption O3 — needs license audit before linking; this ADR does not unblock that. |
| **DeepFilterNet** (DF2 / DF3) | **TFLite + XNNPACK**, after one-time ONNX → TFLite conversion | FP16, INT8 deferred | 8–10 MB (ONNX size; TFLite typically ±20%) | Upstream ships ONNX (`DeepFilterNet{2,3}_onnx.tar.gz`) and PyTorch, **not TFLite**. We convert ONNX → TFLite via `onnx2tf` at vendor-fetch time (one-shot, tracked in `scripts/fetch-vendor.sh`). DeepFilterNet's GRU + grouped-conv graph is well-supported by both `onnx2tf` and XNNPACK; the conversion is mechanical. **Rationale for not picking ONNX RT instead:** DeepFilterNet is the *only* TFLite-incompatible candidate of the three; standing up an entire second runtime to support one model adds ~5 MB binary and a parallel inference-thread story for marginal gain. The conversion cost is bounded; the runtime cost is permanent. License: MIT or Apache-2.0 (per `vendor/deepfilternet/LICENSE-{MIT,APACHE}`). |

**GTCRN** and **U-Net RES** (the reserved fallbacks) inherit the DTLN-AEC slot: TFLite + XNNPACK, FP16 default. If they enter the chain they enter under the same runtime budget.

### 4. Binary-footprint budget

| Component | Budget for `libecnr_pipeline` (linux-aarch64, release) |
|---|---:|
| TFLite static + XNNPACK | **≤ 2.5 MB** (TFLite static is ~2.0 MB; XNNPACK adds ~400 KB; we accept slight slop for symbol bloat) |
| Selected neural model (DTLN-AEC 128_1 + NKF-AEC weights) | **≤ 2.2 MB** (1.99 MB TFLite + ~30 KB raw NKF) |
| Eigen headers (NKF hand-rolled) | header-only, no .so contribution |
| **Total neural-stack overhead** | **≤ 5 MB** above the Phase-0.5 baseline |
| **Hard ceiling for the whole `libecnr_pipeline`** | **15 MB** (Phase-0.5 baseline today is ~6 MB; AEC3 + RNNoise + chain code; remaining headroom is 9 MB; neural eats 5 MB; 4 MB left for model-tier upgrades) |

If a candidate model would push us over the ceiling, **the model loses, not the budget**. DeepFilterNet at 36 MB for the `_ll` ONNX variant is rejected on this rule alone; only the standard ~8 MB variants are eligible.

### 5. Phase 3 entry criteria (when do we wire this)

Phase 3 (neural integration) **does not start** until all of the following are green:

1. **Phase 1 closed:** A55 cross-compile of the Phase-0.5 baseline (`libecnr_pipeline` with AEC3 + RNNoise) runs on the U300 dev kit, RTF < 0.5, no thermal throttle in 10-minute soak. (ADR-0001 A7.)
2. **Phase 2 corpus delivered:** in-cabin recordings across the test vehicles, with reference (post-DRC/EQ tap per ADR-0005) and microphone aligned. Without this we have nothing to evaluate the neural stage *against*; running it on synthetic AEC-Challenge data only re-confirms the upstream paper.
3. **License audit complete** for whichever model enters the chain (open assumption O3 below).
4. **ADR-0009 (media-aware AEC) decided** for whether the neural post-filter is bypassed during music render. The neural stage's behaviour on music-as-echo is otherwise undefined.

If any of (1)–(4) is amber, Phase 3 stays parked.

## Options considered

### Option A — TFLite + XNNPACK (chosen)

| Dimension | Assessment |
|---|---|
| Complexity (integration) | Medium — one runtime, well-documented C++ API, Bazel/CMake builds |
| Cost (engineering) | Medium — one-time `onnx2tf` step for DeepFilterNet, hand-rolled NKF |
| A55 perf | Best of the three — XNNPACK is **explicitly Cortex-A55/A75 tuned** per Arm + Google docs (`docs/Cellular Audio Processing Solutions Deep Dive.md` and Arm developer materials) |
| Binary footprint | ~2.5 MB runtime |
| Determinism | High — TFLite interpreter is fully deterministic given fixed input; FP16/INT8 reproducible across A55 cores |
| Path to DSP offload | Cleanest — TFLite is the lingua franca for HiFi DSP NN compilers (Cadence XAF, CEVA CDNN both ingest TFLite); future Phase 6 can lift the same `.tflite` artefacts |
| Risk | Low — ships in PulseAudio, PipeWire NN plugins, every Android SoC vendor's audio HAL |

**Pros:**
- Empirically the format DTLN-AEC ships in. Zero conversion risk for the lead candidate.
- XNNPACK has named A55 optimisation work upstream (vs. ONNX RT's mainline NEON path which is more generic).
- Smallest binary of the off-the-shelf options.
- Path to DSP offload is preserved (future ADR-0008): TFLite → vendor NN compiler is the standard automotive flow.

**Cons:**
- Doesn't natively run NKF-AEC (handled by raw weights, not a runtime gap).
- Requires `onnx2tf` step for DeepFilterNet (one-shot, tracked).
- INT8 PTQ accuracy on AEC residuals is unverified (open assumption O1).

### Option B — ONNX Runtime

| Dimension | Assessment |
|---|---|
| Complexity | Medium — comparable C++ API |
| Cost (engineering) | Higher — two of three candidate models need conversion *to* ONNX (NKF: PyTorch→ONNX is messy for the complex-GRU loop; DTLN: TFLite→ONNX is non-trivial) |
| A55 perf | Adequate — generic NEON kernels; less A55-specific tuning published than XNNPACK |
| Binary footprint | ~5–10 MB static (mobile build) |
| Determinism | High |
| Path to DSP offload | Worse — DSP NN compilers prefer TFLite; ONNX→TFLite re-conversion at offload time |
| Risk | Medium — the runtime is fine; the conversion debt is the risk |

**Pros:** DeepFilterNet ships ONNX natively. Broader format coverage.
**Cons:** We'd be paying ONNX RT's binary size for one model out of three, while *adding* conversion debt for the other two. Path to Phase-6 DSP offload is worse. Net loss vs. Option A.

### Option C — Raw weights / hand-rolled inference for everything

| Dimension | Assessment |
|---|---|
| Complexity | Low (per model) but linear in model count |
| Cost (engineering) | High — every model is a custom port and every model is a maintenance burden when upstream updates |
| A55 perf | Potentially best — full control over NEON intrinsics, memory layout, frame-streaming |
| Binary footprint | Smallest — tens of KB total |
| Determinism | Highest |
| Path to DSP offload | Impossible without re-port |
| Risk | High — every new model is a project; engineering bandwidth eaten on infrastructure rather than tuning |

**Pros:** Smallest binary, full control, viable for NKF-AEC at 5.3 K params.
**Cons:** DTLN-AEC at <1 M params and DeepFilterNet at ~2 M params are too large to maintain by hand; we'd be re-implementing TFLite's interpreter loop every time a model changes. Picking this for the whole stack is the classic "premature platform" trap — saves 2 MB binary, costs three engineer-quarters.

## Trade-off analysis

The decision is the **median** of the three: TFLite for what TFLite is good at, raw weights only where they make the strict case (NKF-AEC is the only candidate where the hand-rolled cost is *less* than the integration cost — every other candidate is the other way round). ONNX RT is rejected not because it's bad but because **standing up two runtimes in `libecnr_pipeline` is a structural cost we don't want to pay for one model**. We'd rather pay a bounded one-time conversion cost (DeepFilterNet ONNX → TFLite via `onnx2tf`) than a permanent ~5 MB and a parallel inference-thread story.

The quantisation plan (FP32 dev → FP16 cross-compile → INT8 prod after spike) deliberately gates the riskiest step (INT8) behind empirical verification. AEC is the wrong place to assume INT8 PTQ "just works" — residual-echo signals are small-magnitude by definition, and quantisation noise floors compete directly with what the post-filter is meant to attenuate.

## Consequences

**What becomes easier:**
- **Cross-platform parity:** TFLite runs identically on macOS-arm64 (dev), linux-x86_64 (CI), and linux-aarch64 (target). The neural stage doesn't add a new "works on host, breaks on device" failure surface.
- **Model conversion pipeline:** one tool (`onnx2tf` for DeepFilterNet; TFLite Converter from PyTorch for any future PyTorch upstream) plus the hand-rolled NKF path. Tracked in `scripts/fetch-vendor.sh` so vendor fetches are deterministic.
- **Phase-6 DSP offload:** the same `.tflite` artefacts feed Cadence HiFi (XAF) or CEVA (CDNN) compilers when/if Phase 6 happens.
- **Binary reproducibility:** TFLite is statically linked into `libecnr_pipeline`; no `dlopen` of vendor-supplied .so files; no version-skew failure mode at runtime.
- **A55 NEON work:** XNNPACK already does the SIMD heavy lifting for the model graph; our NEON budget is freed for the classical-DSP work (FFT, FIR, resampler) where it has the most marginal value (per `docs/Cellular Audio Processing Solutions Deep Dive.md` Cortex-A55 section).

**What becomes harder:**
- **Binary size:** +2.5 MB for TFLite + XNNPACK is real. If U300 product imposes a tighter `libecnr_pipeline` size budget than 15 MB, we revisit. Today we don't have a number from product.
- **XNNPACK op coverage edge cases:** if a future model lands an unusual op (custom complex-valued conv, non-standard attention) we either get TFLite reference-kernel speed (slow) or hand-roll the op. Logged via the one-time warning above.
- **INT8 PTQ may regret on AEC residuals:** explicitly called out as open assumption O1; Phase 3 spike addresses it.
- **`onnx2tf` is a runtime-tools dep at vendor-fetch time:** one more Python toolchain dep in `scripts/fetch-vendor.sh`. Bearable. Pinned version in the script.
- **Maintaining the hand-rolled NKF code:** if NKF's upstream `nkf_epoch70.pt` weights are retrained or the architecture changes, our port has to follow. The port is 5.3 K params, not 5.3 M; cost is bounded.

**What we explicitly defer:**
- **QAT (quantisation-aware training):** out of scope for v1. Phase 4+ if PTQ is insufficient.
- **NPU / DSP delegate:** ADR-0008 owns this. TFLite is chosen *partly* because every plausible NPU/DSP delegate (Arm NN, Hexagon, HiFi NN, Ethos-U) consumes TFLite.
- **Multi-model ensemble:** the chain runs *one* neural post-filter at a time, switched by the mode controller. Not running NKF + DTLN + DeepFilterNet concurrently. Picked at runtime via config.
- **Float-frame interface:** `Frame` is mono int16 today (ADR-0006). The runtime's input/output is float32 (or quantised int8); the boundary stays inside `ecnr::nn::Runtime::Process` and does the int16↔float conversion there. When `Frame` widens to float (gated on ADR-0003 fullband path), the conversion drops.

## Open assumptions (NOT yet validated)

These are the project-risk items that this ADR knowingly does not resolve. Each is a Phase-3 spike or its own future ADR.

### O1. INT8 PTQ preserves AEC quality at the post-filter working point

**Assumption:** Post-training INT8 quantisation of DTLN-AEC and DeepFilterNet preserves ERLE / residual-echo-likelihood / PESQ within an acceptable delta of FP32.
**Status:** **Unvalidated.** AEC residuals are small-magnitude; INT8 noise floor competes with the signal the post-filter is meant to clean. Published INT8 PTQ wins are mostly for keyword spotting and image classification, where signal magnitude is well above quantisation noise.
**Validation:** Phase-3 spike: PTQ both models with `tflite::PostTrainingQuantization`, run on AEC-Challenge dev set + Phase-2 cabin corpus, compute ERLE/`residual_echo_likelihood`/PESQ delta vs FP32.
**Mitigation if wrong:** Stay on FP16 for that model; FP16 budget is well within the 15 MB ceiling.
**Recommended:** Phase-3 task; not a separate ADR.

### O2. XNNPACK op coverage is complete for the chosen models on A55

**Assumption:** Every op in DTLN-AEC's `.tflite` graph and the `onnx2tf`-converted DeepFilterNet graph has an XNNPACK NEON kernel.
**Status:** **Unvalidated.** XNNPACK covers the 80% common-case (FC, depthwise, conv, GRU-unrolled, basic activations); if the model uses a custom layer, INT16 quantisation, or a non-standard reduction, fall-back to TFLite reference is slow.
**Validation:** First Phase-3 milestone is to load each candidate `.tflite` on macOS-arm64, log the per-op delegate decision (XNNPACK vs reference), and identify any reference-kernel ops on the hot path.
**Mitigation if wrong:** Either re-author the model (request from upstream if license permits) or accept the slower op for v1.
**Recommended:** Phase-3 task; not a separate ADR.

### O3. License terms permit shipping the chosen models in `libecnr_pipeline`

**Assumption:** Each candidate's licence allows redistribution in a closed-source product binary.
**Status:** **Partial.** NKF-AEC is BSD-3 (confirmed in `vendor/nkf-aec/src/nkf.py`). DeepFilterNet is dual MIT / Apache-2.0 (confirmed in `vendor/deepfilternet/LICENSE-{MIT,APACHE}`). DTLN-AEC's licence file (`vendor/dtln-aec/LICENSE`) needs explicit audit — the research report flagged it as "待核验" (to be verified). The pretrained weights in `pretrained_models/` may have a different licence than the code.
**Validation:** ADR-0001 has this as a process action item; flagged here as a hard prerequisite for Phase 3.
**Mitigation if wrong:** Drop the offending model from the candidate set; GTCRN and U-Net RES exist as fallbacks.
**Recommended:** Track on the ADR-0001 action list; this ADR records the dependency.

### O4. The 15 MB `libecnr_pipeline` ceiling is acceptable to U300 product

**Assumption:** The product accepts a ~15 MB shared library for the audio chain.
**Status:** **No number from product yet.** ADR-0001 doesn't quantify it. Cellular handsets with full APM + neural NS routinely sit at 20–40 MB on the audio side, so 15 MB is *plausible* but unconfirmed for the U300 form factor.
**Validation:** Conversation with U300 platform team; this is the same conversation that locks the render-tap question (ADR-0005).
**Mitigation if wrong:** Drop to FP16 only (no INT8 model variant), drop DeepFilterNet from the candidate set, prefer DTLN-AEC 128_1 (1.99 MB). At minimum, the chain still ships with NKF-AEC raw-weight only at ~30 KB.
**Recommended:** Surface in Phase-1 product conversation, alongside ADR-0005.

### O5. `onnx2tf` conversion is stable for DeepFilterNet's specific graph

**Assumption:** `onnx2tf` (current version, pinned in `scripts/fetch-vendor.sh`) successfully converts `DeepFilterNet{2,3}_onnx.tar.gz` to a working TFLite without graph-rewrite breakage.
**Status:** **Unvalidated.** DeepFilterNet's deep-filtering blocks use complex-valued ops in some variants; `onnx2tf` typically expands these to real ops. Mostly works; occasionally introduces a precision hit.
**Validation:** Phase-3 first task: run the conversion on macOS dev host, run the resulting TFLite on AEC-Challenge dev set, compare to upstream Rust binary.
**Mitigation if wrong:** Use the deeper-filterless DeepFilterNet variant, drop to the DTLN-AEC tier, or — only as last resort — add ONNX RT alongside TFLite for this single model.
**Recommended:** Phase-3 task.

## Action items

- [ ] **Phase 1 (cross-compile spike):** confirm TFLite 2.16.x + XNNPACK static build cross-compiles for `aarch64-linux-gnu`. Pin the exact build recipe in `cmake/external/tflite.cmake`. Owner: project lead. Due: end of Phase 1.
- [ ] **Phase 1 (binary budget):** measure the Phase-0.5 baseline `libecnr_pipeline` size (AEC3 + RNNoise) on linux-aarch64 to confirm the 6 MB starting figure used in this ADR. Update the budget table if it's wrong. Due: end of Phase 1.
- [ ] **Phase 1 product conversation (joint with ADR-0005):** confirm or revise the 15 MB ceiling with the U300 platform team (closes O4).
- [ ] **Phase 2 (cabin corpus):** ensure recordings cover the input scenarios needed to evaluate the neural post-filter (single-talk far-end, double-talk, music-as-echo, road noise sweep). Without this we cannot run the O1 spike.
- [ ] **Phase 3 task 1 — runtime stand-up:** add `ecnr::nn::Runtime` interface; wire TFLite + XNNPACK behind it; load `dtln_aec_128_1.tflite` end-to-end on macOS-arm64 with a unit test that round-trips a 10 ms frame.
- [ ] **Phase 3 task 2 — NKF hand-roll:** port `vendor/nkf-aec/src/nkf.py` to C++ against Eigen; bit-exact match against PyTorch reference within 1e-5 RMS on `vendor/nkf-aec/src/{ref,mic}.wav`.
- [ ] **Phase 3 task 3 — DeepFilterNet conversion:** `onnx2tf` step in `scripts/fetch-vendor.sh`; verify on AEC-Challenge dev set vs upstream Rust binary (closes O5).
- [ ] **Phase 3 task 4 — quantisation spike:** PTQ DTLN-AEC and DeepFilterNet to INT8 + FP16; compute ERLE/`residual_echo_likelihood`/PESQ deltas on Phase-2 corpus; write the result back to this ADR (closes O1).
- [ ] **Phase 3 task 5 — XNNPACK op coverage:** log per-op delegate decision for every candidate `.tflite` on macOS dev host; flag reference-kernel hot-path ops (closes O2).
- [ ] **License audit (process action, ADR-0001 carry-over):** explicit licence determination for DTLN-AEC code and weights; merge result into this ADR (closes O3).
- [ ] **ADR-0008 (DSP offload) input:** when ADR-0008 is opened, link to the per-model TFLite artefacts as the offload-candidate inputs.

## Sketch — `ecnr::nn::Runtime` interface

This is the **shape** Phase 3 will implement against. No `tflite::` symbols on the public surface.

```cpp
#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace ecnr::nn {

// Tag for which backend the runtime instance uses. The chain selects per-model
// at config time; never per-frame.
enum class Backend {
  kTfliteXnnpack,    // DTLN-AEC, DeepFilterNet (post-conversion), GTCRN, U-Net RES
  kRawNkfAec,        // NKF-AEC only — hand-rolled against Eigen
};

// Single-frame neural post-filter. One Runtime instance per model; the chain
// holds at most one active Runtime at a time (mode controller swaps).
class Runtime {
 public:
  static std::unique_ptr<Runtime> Load(Backend backend, std::string model_path);

  virtual ~Runtime() = default;

  // 10 ms frame in/out. Lengths must equal the model's configured rate
  // (160 samples for 16 kHz tier, 480 for 48 kHz tier — see ADR-0003).
  // Implementation owns the int16<->float conversion.
  virtual void Process(std::span<const int16_t> in, std::span<int16_t> out) = 0;

  // Drop streaming state (GRU hidden state, Kalman filter state for NKF, etc.).
  // Called on stream restart.
  virtual void Reset() = 0;
};

}  // namespace ecnr::nn
```

The chain's neural stage holds a `std::unique_ptr<Runtime>`; the choice of backend is set at `AecChain::Init` time via config and never changes for the lifetime of the chain instance. This mirrors ADR-0006's "config is set once" stance.

## References

- [docs/adr/0001-hybrid-aec-architecture-review.md](0001-hybrid-aec-architecture-review.md) — architecture decision and action item #6 that this ADR closes.
- [docs/adr/0003-canonical-sample-rate.md](0003-canonical-sample-rate.md) — 16 / 48 kHz tiering; affects which model variants are eligible.
- [docs/adr/0005-render-tap-policy.md](0005-render-tap-policy.md) — defines "smart-amp residual at typical playback levels → feeds ADR-0007 sizing"; ADR-0005 is the upstream input that defines what the neural post-filter is asked to absorb.
- [docs/adr/0006-aec-chain-interface.md](0006-aec-chain-interface.md) — `AecChain` interface; sets the precedent of hiding the runtime backend behind `Impl`.
- [docs/deep-research-report.md](../deep-research-report.md) — algorithm comparison table (lines 86–105), production-stack recommendation (line 174 — "经典 AEC + 轻量神经 RES/NS, 优先评估 NKF-AEC ... DTLN-AEC"), Cortex-A55 strategy (lines 134–138 — XNNPACK A55/A75 tuning).
- [docs/Cellular Audio Processing Solutions Deep Dive.md](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — Cortex-A55 section, hybrid DSP-DNN paradigm shift (lines 58–64), ULCNet ultra-low-complexity reference, CDNN compiler reference for future DSP offload (line 143).
- [vendor/dtln-aec/pretrained_models/](../../vendor/dtln-aec/pretrained_models/) — six TFLite variants (`dtln_aec_{128,256,512}_{1,2}.tflite`, 1.99–24.16 MB).
- [vendor/dtln-aec/run_aec.py](../../vendor/dtln-aec/run_aec.py) — upstream runner; reference for input/output framing.
- [vendor/nkf-aec/src/nkf.py](../../vendor/nkf-aec/src/nkf.py) — model definition (5.3 K params; complex GRU + Kalman loop).
- [vendor/nkf-aec/src/nkf_epoch70.pt](../../vendor/nkf-aec/src/nkf_epoch70.pt) — 28 KB PyTorch weights file; the source for the hand-rolled port.
- [vendor/deepfilternet/models/](../../vendor/deepfilternet/models/) — `DeepFilterNet{2,3}_onnx.tar.gz` (~8 MB each); `_ll` variants rejected on binary budget.
- [vendor/deepfilternet/LICENSE-{MIT,APACHE}](../../vendor/deepfilternet/) — dual MIT / Apache-2.0.
- TFLite (LiteRT) C++ API — `tensorflow/lite/interpreter.h` upstream; pinned 2.16.x in the future `cmake/external/tflite.cmake`.
- XNNPACK — `google/XNNPACK` upstream; A55/A75 NEON kernel tuning notes in the project README.
- `onnx2tf` — `PINTO0309/onnx2tf` upstream; pinned version to be added to `scripts/fetch-vendor.sh` when Phase 3 starts.

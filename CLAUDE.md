# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository nature

ECNR is the **automotive in-cabin AEC + NS audio stack for the U300 terminal** (Cortex-A55, Yocto Linux, VoLTE/VoNR): a C++17 processing chain (`src/`), an offline bench + eval harness, Python scoring/gating tooling (`reference/`), and the research corpus the project grew out of (`docs/`). `PROJECT.md` is the live state document — mission, roadmap, decisions log, known limitations.

## Build & test

```sh
scripts/fetch-vendor.sh            # one-time: vendored deps into vendor/ (pinned by vendor/MANIFEST.tsv)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DECNR_BUILD_LIVE=OFF
cmake --build build -j
ctest --test-dir build             # gtest units
```

Deps: C++17 compiler, cmake + ninja + meson, pkg-config, libsndfile, speexdsp, abseil (brew/apt). `ecnr_live` (host loopback) needs CoreAudio/ALSA — keep `ECNR_BUILD_LIVE=OFF` for headless work. Cross-compile for aarch64: `scripts/cross-build-yocto/build.sh` (Docker; `--smoke` runs a qemu-user sanity pass).

## Chain architecture (src/pipeline/)

Beamformer (DSB/passthrough) → WebRTC AEC3 + HPF (APM #1) → RNNoise NS (int8 path; 16 k tier resamples 16↔48 k via SpeexDSP) → WebRTC AGC2 (APM #2, post-NS per ADR-0001, **on by default since v0.4.1**). 10 ms frames, 16 k/48 k two-tier (ADR-0003), 2–8 mics (ADR-0004). All `webrtc::` types stay behind pimpl adapters.

## Quality gates — run before claiming any chain change is safe

1. **AEC-Challenge perceptual gate (enforced, also in CI):**
   `python3 reference/run_aec_challenge.py --bench ./build/ecnr_bench --dnsmos-model models/dnsmos_p835.onnx --aecmos-model models/aecmos.onnx --out-dir /tmp/gate`
   30-clip pinned corpus (`datasets/aec_challenge/MANIFEST.tsv`, fetched by `reference/fetch_aec_challenge.py`); floors per ADR-0012 §2.1 v2 + §3.1 applicability matrix. Exit 0 = PASS.
2. **GB/T 45314 eCall pre-compliance gate (China market, ADR-0013/0014):**
   `python3 reference/gen_gbt45314_ecall_conditions.py && ./build/ecnr_eval --run --conditions conditions/gbt45314_ecall --res-models models --res-units 256 --ns-vad-blend 0.20,1.0 --no-agc --out /tmp/gbt.csv && python3 reference/check_gbt45314_ecall_gate.py --in-csv /tmp/gbt.csv`
   (RES models: `python3 reference/fetch_res_models.py`; needs the optional onnxruntime build — brew/`ORT_HOME`.)
   Current verdict: **floors met** (exit 2 — headroom targets pending) at the eCall RC preset above, via the ADR-0014 DTLN/AEC3 hybrid; DT was the historic blocker. Don't "fix" the gate; fix the chain.
3. Per-stage CPU split prints on the bench summary line (`cpu_aec/cpu_ns/cpu_agc`); perf changes get logged in `docs/perf/a55-optimization-log.md` with gate results.

## Conventions

- ADRs under `docs/adr/` are the decision record; the acceptance bar (ADR-0012) numbers are mirrored in `run_aec_challenge.py`, `sweep_ns_blend.py`, `check_acceptance_bar.py` — keep in sync manually.
- Version tags bump the **patch digit only** (v0.4.1 → v0.4.2) unless the user explicitly asks otherwise.
- Vendored sources are not committed (fetch on demand); datasets/models are SHA-pinned by manifests, WAVs gitignored.
- Config knobs that bake into APM at Init (`SetAgcMaxGainDb`, `SetAecFilterLengthBlocks`) must be called before `AecChain::Init`.

## Research docs (docs/)

The original research reports remain canonical reading: `docs/deep-research-report.md` (CN — keep its Simplified-Chinese-prose + English-term bilingual style; don't translate technical terms) and `docs/Cellular Audio Processing Solutions Deep Dive.md` (EN). Both use inline `citeturnNsearchM` / `urlFOOturnN` reference-tracker tokens — preserve them verbatim unless asked to clean up. `docs/5G_VoNR_Audio_Architecture.pdf` is ~20 MB — read specific page ranges only.

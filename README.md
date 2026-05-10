# ECNR

Automotive in-cabin AEC + NR audio stack for the U300 system. Hybrid architecture: WebRTC AEC3 linear backbone + neural post-processing for residual echo and noise.

See [PROJECT.md](PROJECT.md) for the architecture, roadmap, and dependency list. See [docs/](docs) for the underlying research.

## Status

Phase 0 (bootstrap) **done**: scaffold + stub AEC/NS backends + dev-host smoke test green. Phase 0.5 (real WebRTC AEC3 + RNNoise wiring) is next. Not yet cross-compiled for A55. Not yet vehicle-validated.

Vendored open-source dependencies are not in git — fetch on demand:

```sh
scripts/fetch-vendor.sh required   # ~5 MB build deps only
scripts/fetch-vendor.sh            # ~450 MB inc. research code + pretrained models
```

## Prerequisites (dev host: Linux or macOS)

- C++17 compiler (clang or gcc)
- CMake ≥ 3.20
- `pkg-config` and `libsndfile`
- Python 3 + `numpy` (only for generating synthetic test WAVs)

macOS:

```sh
brew install cmake pkg-config libsndfile
```

Linux (Debian/Ubuntu):

```sh
apt install cmake pkg-config libsndfile1-dev build-essential python3-numpy
```

`googletest` is fetched at configure time — no separate install needed.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

## Run the smoke test

```sh
ctest --test-dir build --output-on-failure
```

Expected (Phase 0): three tests pass under stub backends; the echo-attenuation test asserts cumulative ERLE > 5 dB. Phase 0.5 will tighten to > 15 dB once AEC3 + RNNoise replace the stubs.

## Run the offline bench harness

```sh
./build/src/bench/ecnr_bench --mic mic.wav --ref ref.wav --out out.wav
```

Prints per-frame RTF and overall ERLE; writes the processed near-end signal to `out.wav`. Inputs must be mono; sample rate is detected (8/16/32/48 kHz). The harness assumes `mic.wav` and `ref.wav` are time-aligned at the start; the AEC's delay estimator handles the rest.

If you don't have real audio yet, generate a synthetic pair:

```sh
python3 reference/gen_synth.py --duration 10 --out-dir reference/synth/
./build/src/bench/ecnr_bench --mic reference/synth/mic.wav --ref reference/synth/ref.wav --out /tmp/out.wav
```

## Layout

| Path | Purpose |
|---|---|
| `docs/` | Background research |
| `vendor/` | Vendored open-source forks (untouched) |
| `src/core/` | Frame, ring buffer, resampler |
| `src/pipeline/` | `AecChain` — AEC3 + RNNoise wire-up |
| `src/hal/` | Mic/render abstraction (file-backed in v1) |
| `src/bench/` | Offline benchmark binary |
| `src/tests/` | gtest |
| `models/` | Neural model artifacts (Phase 3+) |
| `reference/` | Test audio |
| `third_party/` | Build-time dependencies |

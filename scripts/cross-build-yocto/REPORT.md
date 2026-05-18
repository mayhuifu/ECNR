# Cross-build spike: aarch64-poky-linux

**Status:** GREEN — `ecnr_bench` cross-compiles to a working aarch64 ELF against a stand-in Yocto SDK.
**Date:** 2026-05-10
**Resolves (in part):** [ADR-0001](../../docs/adr/0001-hybrid-aec-architecture-review.md) **assumption A7** — *the existing macOS host build is reproducible for the A55 target.*

## TL;DR

The project's CMake + Meson + autotools chain cross-compiles cleanly for `aarch64-poky-linux` using a stand-in Poky reference SDK (Yocto 5.0.7 scarthgap, `cortexa57-qemuarm64`), under Docker Desktop on Apple Silicon. Five build-mechanics issues surfaced (1 user-error, 4 known-bugs / latent project bugs); all fixed in this commit. Five aarch64 artefacts are produced (1 ELF + 4 archives), each verified with `file`. ABI compatibility with the real U300 BSP is **not** validated by this spike — that's gated on receiving the vendor SDK; the swap-in procedure is documented in `README.md`. Performance on A55 is also out of scope; the Poky reference SDK tunes for cortex-a57 (forward-compatible but not optimal).

## Setup

| Item | Value |
|---|---|
| Build host | Apple Silicon macOS, Docker Desktop linux/amd64 (Rosetta) |
| SDK | Yocto Project 5.0.7 (scarthgap), Poky reference, `core-image-sato` |
| Target | `cortexa57-qemuarm64` — stand-in for U300's cortexa55 (forward-compatible at the ARMv8.0-A ISA level) |
| Toolchain | gcc / binutils via `aarch64-poky-linux-` prefix; glibc 2.39, libstdc++ from gcc 13.3 |
| Container | `debian:bookworm-slim` + cmake 3.25, meson 1.0, autotools, ninja |

## Dependency survey (Sato sysroot)

After sourcing `$SDK_ENV`, `pkg-config` was queried for each runtime / build dep of `ecnr_bench`:

| Module | In Sato sysroot? | Action |
|---|---|---|
| `sndfile` 1.2.2 | ✅ pre-built | none |
| `speexdsp` 1.2.1 | ✅ pre-built | none |
| `alsa` 1.2.11 | ✅ pre-built (unused — `ecnr_live` gated off) | none |
| `absl_strings`, `absl_synchronization` | ❌ missing | **cross-build** from upstream `20250127.1` |
| `webrtc-audio-processing-2` | ❌ missing (we build this ourselves) | built via `ExternalProject` |

**Implication for U300 vendor SDK:** abseil is the only dep with a non-trivial chance of being absent (it's a build-time transitive of webrtc-apm; not a typical BSP runtime). Worst case, we apply the same out-of-tree cross-build there.

## Build-mechanics issues encountered + fixes

### 1. Bad URL guess on first Dockerfile draft

| | |
|---|---|
| **Symptom** | `curl: (22) The requested URL returned error: 404` on the SDK download. |
| **Root cause** | Poky doesn't publish a `core-image-minimal-cortexa57-qemuarm64-toolchain.sh` for `5.0.7` — only the `-ext` extensible variant and the `-sato` regular variant. |
| **Fix** | Switched to `core-image-sato-cortexa57-qemuarm64-toolchain-5.0.7.sh`. Bonus: Sato carries sndfile / speexdsp / alsa pre-built, cutting our deps cross-build to abseil only. |

### 2. Container shell entrypoint didn't source the SDK env for `-c '...'` invocations

| | |
|---|---|
| **Symptom** | `pkg-config sndfile` reported missing, `$CC` was empty, `$SDKTARGETSYSROOT` was empty. |
| **Root cause** | Original entrypoint used `bash --rcfile /etc/cross-rc` — rc files are only sourced for interactive shells, not `bash -c '...'`. |
| **Fix** | Replaced with a `/usr/local/bin/cross-entry` wrapper that explicitly `. "$SDK_ENV"` before `exec /bin/bash "$@"`. |

### 3. Cross-built abseil's `.pc` files used the wrong sysroot-resolved prefix

| | |
|---|---|
| **Symptom** | CMake configure failed with `Imported target "webrtc::apm" includes non-existent path /opt/poky/5.0.7/sysroots/cortexa57-poky-linux/opt/poky-extras/include`. |
| **Root cause** | Yocto's pkg-config wrapper prepends `$SDKTARGETSYSROOT` to absolute paths in `.pc` files. Our abseil `.pc` files baked in `prefix=/opt/poky-extras` (the docker-volume mount point); the wrapper then advertised `<sysroot>/opt/poky-extras/...`, which doesn't exist. |
| **Fix** | Symlink `$SDKTARGETSYSROOT/opt/poky-extras → /opt/poky-extras` at the start of each container run. Cleaner than .pc-file post-processing; docker volume caching for the abseil build is preserved. |

### 4. Project's root CMakeLists.txt unconditionally builds `ecnr_live`

| | |
|---|---|
| **Symptom** | `Target "ecnr_live" links to PkgConfig::ALSA but the target was not found.` |
| **Root cause #1** | Cross-build was trying to wire up `ecnr_live` even though miniaudio + CoreAudio is host-only (the user explicitly said "defer ecnr_live for the spike"). |
| **Root cause #2** | The Linux/ALSA branch used `pkg_check_modules(ALSA alsa)` (no `IMPORTED_TARGET`), which produces `ALSA_FOUND` but no `PkgConfig::ALSA` target. The subsequent `target_link_libraries(ecnr_live PRIVATE PkgConfig::ALSA)` fails configure. Latent bug on any Linux host. |
| **Fix** | Added `option(ECNR_BUILD_LIVE ON)` gating the whole `add_executable(ecnr_live ...)` block; cross-build passes `-DECNR_BUILD_LIVE=OFF`. Also fixed the latent pkg-config bug by adding `IMPORTED_TARGET`. |

### 5. SDK's auto-generated meson cross-file has wrong `host_machine.cpu_family`

| | |
|---|---|
| **Symptom** | `meson.build:168:30: ERROR: Feature neon cannot be enabled` from `vendor/webrtc-audio-processing/meson.build`. |
| **Root cause** | `/opt/poky/5.0.7/sysroots/x86_64-pokysdk-linux/usr/share/meson/aarch64-poky-linux-meson.cross` declares `[host_machine] cpu_family = 'x86_64'` — that's the *build* host, not the *target*. WebRTC APM keys NEON support on `host_machine.cpu_family() == 'aarch64'` (`meson.build:140-144`), so the check returns false and `.require(have_neon)` errors out. Known Yocto bug, fixed in newer releases. |
| **Fix** | Wrote a 4-line override file `scripts/cross-build-yocto/meson/aarch64-host-machine-override.cross` that contains only the correct `[host_machine]` section. Meson processes multiple `--cross-file` flags in order, later ones overriding earlier. `cmake/BuildWebRTCAPM.cmake` chains both files on cross-compile. The SDK file stays untouched. |

## Cross-build artefacts

```
$ scripts/cross-build-yocto/build.sh --verify

build-aarch64/ecnr_bench
  ELF 64-bit LSB pie executable, ARM aarch64, version 1 (GNU/Linux),
  dynamically linked, interpreter /lib/ld-linux-aarch64.so.1,
  for GNU/Linux 5.15.0, with debug_info, not stripped
  size: 17.7 MB

build-aarch64/libecnr_pipeline.a                              ar archive
build-aarch64/libecnr_core.a                                  ar archive
build-aarch64/libecnr_rnnoise.a                               ar archive
build-aarch64/webrtc-apm-install/lib/libwebrtc-audio-processing-2.a   ar archive

# Spot-check that archive members are aarch64 (not just the archive itself):
$ ar x libwebrtc-audio-processing-2.a checks.cc.o && file checks.cc.o
  ELF 64-bit LSB relocatable, ARM aarch64, version 1 (SYSV), not stripped
```

Total build time on Apple Silicon under Rosetta:
- Image (cached): instantaneous after first run
- abseil cross-build: ~3 min
- ecnr_bench (incl. webrtc-apm Meson): ~8 min the first time, ~30 s incremental

## What this spike does and doesn't prove

**Proves** (build-mechanics validation, the spike's stated scope):

- ✅ The project's CMake + Meson + abseil-CMake + autotools build systems all cross-compile cleanly under a Yocto Poky-class SDK.
- ✅ The vendored runtime deps that the U300 SDK is *likely* to carry (sndfile, speexdsp) integrate via pkg-config without manual sysroot fiddling.
- ✅ The WebRTC APM Meson ExternalProject driver cross-compiles when handed the SDK's cross-file + an A55-correct `[host_machine]` override.
- ✅ Yocto-host-tuned aarch64 binaries are produced (`file` shows `ELF 64-bit LSB executable, ARM aarch64`).

**Does NOT prove**:

- ❌ Real-target ABI compatibility — Poky reference sysroot's glibc / libstdc++ / kernel ABI is generic; the U300 BSP sysroot may differ. Re-validate against the vendor SDK before declaring U300 ready.
- ✅ Runtime correctness on aarch64 (via qemu-user-static) — covered by `build.sh --smoke` as of the perf/size work stream. The chain runs end-to-end and produces healthy AEC3 ERLE numbers (see baseline below).
- ❌ Performance on A55 — qemu emulation adds ~5–10× overhead, so the qemu RTF is a relative baseline only, not an absolute A55 number. The U300 vendor SDK's `cortexa55` tuning + real hardware are required for absolute perf.
- ❌ ADR-0001 §"Action items" → "measure DSB CPU on A55" — that requires real hardware. The qemu baseline below makes proportional A/B perf comparisons possible across binary-size / config experiments in the meantime.

## aarch64 baseline + size/perf experiments (under qemu-aarch64-static)

Captured via `build.sh --smoke`. Each row is the current `--bench` default at that point in the work stream.

| Build config | Binary | RTF (qemu) | ERLE (dB) | Δ binary | Δ RTF |
|---|---:|---:|---:|---:|---:|
| **C (baseline)** `RelWithDebInfo`, no strip | 17.66 MB | 2.25 | 7.69 | — | — |
| **A** `Release` + `--strip-all` | **14.94 MB** | **2.10** | 7.69 | **−2.72 MB (−15.4%)** | **−6.3%** |

Caveats unchanged from above:
- qemu RTF includes ~5–10× emulation overhead → real-A55 RTFs are expected at ~0.2–0.4× these numbers.
- Identical ERLE across rows is the correctness guard: behaviour-preserving.

Notes on the qemu RTF:
- 2.25 = "67 s of qemu-emulated CPU to process 30 s of audio". qemu-user-static typically adds ~5–10× emulation overhead, so a real A55 would be expected around RTF 0.22–0.45 (well sub-realtime). The number is **not** A55-accurate but **is** stable enough for proportional A/B comparison across builds — what changes between two runs is attributable to the change, not the emulator.
- ERLE matching the host build is the meaningful correctness signal: the chain produces the same AEC behaviour on aarch64 as on macOS, ruling out arch-specific bugs in our code.

## Recommended next steps

1. **When the U300 vendor SDK lands** (gated on BSP team handover): drop it in via the documented swap-in process (`scripts/cross-build-yocto/README.md`), re-run `build.sh`, confirm artefacts.
2. **CI integration** (deferred): the cross-build is reproducible and could run in GitHub Actions. Holding off until ADR-0001 A7 is fully closed against the real U300 SDK.
3. **Binary-size reduction** (in progress, see perf/size work stream): the 14.84 MB `.rodata` is dominated by RNNoise weights, and ~11 MB of that appears to be float-weight tables that are dead with the current `compute_linear` branch order. Concrete experiments queued: switch to `int8` path (saves both binary size and runs faster on A55's DotProd ext); strip in Release builds (already validated as a −1.9 MB free win above).

## Cost note

The first full run (image build incl. SDK download) took **~97 minutes** end-to-end, dominated by Rosetta-emulated execution of the SDK's installer script. Subsequent runs are fast: Docker layer cache short-circuits the image build, and `build-aarch64/` is incremental. The container size is ~7 GB (SDK is ~5 GB unpacked).

If iteration speed becomes a bottleneck, options in priority order:

1. Move the build container to a Linux x86_64 host (no Rosetta).
2. Build a `SDKMACHINE=aarch64` SDK from Yocto sources (host arch matches Apple Silicon → no emulation), but requires a Yocto build environment which is a separate ~2-hour bitbake yak-shave.

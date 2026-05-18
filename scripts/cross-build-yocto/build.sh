#!/usr/bin/env bash
# Cross-build driver for the Yocto SDK spike.
#
# Builds the docker image (idempotent), then runs the chain inside the
# container with the project repo bind-mounted at /work. The chain is:
#
#   1. (one-time per container) cross-build libsndfile + speexdsp from
#      upstream tarballs into the SDK's "extra" prefix
#      ($SDK_INSTALL_PATH/sysroots/cortexa57-poky-linux/usr/local). These
#      runtime deps are linked by ecnr_hal (sndfile) and the vendored
#      WebRTC APM (speexdsp via the project's CMake glue). The U300
#      vendor SDK is expected to carry these in the BSP sysroot; this
#      step bridges the gap for the Poky reference SDK.
#
#   2. Configure + build ecnr_bench with CMake using the SDK's
#      cmake-wrapper. webrtc-audio-processing (Meson) and rnnoise are
#      sub-projects of the main CMake build; both pick up the
#      cross-compile config from the sourced environment.
#
#   3. Verify the produced artefacts are aarch64 ELF.
#
# Usage:
#     scripts/cross-build-yocto/build.sh            # full chain
#     scripts/cross-build-yocto/build.sh --shell    # interactive shell
#     scripts/cross-build-yocto/build.sh --deps     # just cross-build deps
#     scripts/cross-build-yocto/build.sh --verify   # just `file` checks
#
# The build container produces output under build-aarch64/ in the host
# repo (already covered by .gitignore's build-*/ pattern).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE_NAME="ecnr-cross-yocto:5.0.7"
CONTAINER_NAME="ecnr-cross-yocto"

mode="${1:-build}"

build_image() {
  echo "==> building $IMAGE_NAME (first run downloads ~300 MB SDK; subsequent runs use Docker layer cache)"
  docker build \
    --platform=linux/amd64 \
    -t "$IMAGE_NAME" \
    -f "$REPO_ROOT/scripts/cross-build-yocto/Dockerfile" \
    "$REPO_ROOT/scripts/cross-build-yocto"
}

run_in_container() {
  # --platform pin matches the image; without it Docker on Apple Silicon
  # tries to launch with the host's native arch and emits a warning.
  docker run --rm \
    --platform=linux/amd64 \
    --name "$CONTAINER_NAME" \
    -v "$REPO_ROOT":/work \
    -v "ecnr-cross-yocto-sdk-extras:/opt/poky-extras" \
    "$IMAGE_NAME" \
    -c "$1"
}

cross_build_deps_script() {
  # The Sato reference sysroot already carries libsndfile (1.2.2),
  # speexdsp (1.2.1), and alsa (1.2.11). What's missing is abseil — a
  # transitive build- and link-time dependency of webrtc-audio-processing.
  # The U300 vendor SDK might or might not ship abseil; we cross-build it
  # into a docker-volume-mounted extras prefix either way so the host
  # build tree stays clean and a cached layer survives container restarts.
  cat <<'INNER'
set -euo pipefail
# Install cross-built deps under /opt/poky-extras (a docker volume, so
# the artefacts persist across container runs) AND symlink into the SDK
# sysroot so Yocto's pkg-config wrapper resolves prefix paths correctly.
#
# Why the symlink: SDK pkg-config rewrites .pc-file `prefix=` paths by
# prepending $SDKTARGETSYSROOT. A .pc file produced by `cmake --install
# /opt/poky-extras` bakes in `prefix=/opt/poky-extras` (absolute), and
# pkg-config then advertises includes under $SDKTARGETSYSROOT/opt/poky-
# extras/include. The symlink makes that path resolve to the actual
# install location, with no .pc-file post-processing required.
EXTRAS=/opt/poky-extras
mkdir -p "$EXTRAS"
mkdir -p "$(dirname "$SDKTARGETSYSROOT/opt/poky-extras")"
ln -sfn "$EXTRAS" "$SDKTARGETSYSROOT/opt/poky-extras"
export PKG_CONFIG_PATH="$EXTRAS/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# --- abseil-cpp ---------------------------------------------------------
# CMake-only build; pin to the LTS branch matching what webrtc-audio-
# processing v2 expects. Build static so a single .a per absl module
# slots in cleanly to the meson Build's pkg-config consumption.
ABSEIL_VERSION=20250127.1
if ! pkg-config --exists absl_strings 2>/dev/null; then
  echo "==> cross-building abseil-cpp ${ABSEIL_VERSION}"
  cd /tmp
  rm -rf abseil-cpp abseil.tar.gz
  curl -fsSL "https://github.com/abseil/abseil-cpp/archive/refs/tags/${ABSEIL_VERSION}.tar.gz" -o abseil.tar.gz
  mkdir -p abseil-cpp && tar -xzf abseil.tar.gz -C abseil-cpp --strip-components=1
  cd abseil-cpp
  rm -rf build && mkdir build && cd build
  # CMAKE_TOOLCHAIN_FILE is auto-picked up from the env-setup script.
  cmake .. \
      -DCMAKE_INSTALL_PREFIX="$EXTRAS" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DABSL_ENABLE_INSTALL=ON \
      -DABSL_PROPAGATE_CXX_STD=ON \
      -DABSL_BUILD_TESTING=OFF \
      -DCMAKE_CXX_STANDARD=17
  cmake --build . -j$(nproc)
  cmake --install .
else
  echo "==> abseil already available (cached or pre-installed in sysroot)"
fi

echo
echo "==> deps prefix populated at $EXTRAS"
ls "$EXTRAS/lib/pkgconfig/" 2>/dev/null | grep -E "^absl" | head -5 || true
echo
echo "==> sysroot already carries (no cross-build needed):"
for m in sndfile speexdsp alsa; do
  printf "  %-12s " "$m"
  pkg-config --modversion "$m" 2>/dev/null || echo "missing!"
done
INNER
}

cross_build_bench_script() {
  cat <<'INNER'
set -euo pipefail
# Re-establish the sysroot symlink each container start (see --deps step
# for the rationale). The /opt/poky-extras volume persists between runs;
# the symlink inside the SDK sysroot does not.
mkdir -p "$SDKTARGETSYSROOT/opt"
ln -sfn /opt/poky-extras "$SDKTARGETSYSROOT/opt/poky-extras"
export PKG_CONFIG_PATH="/opt/poky-extras/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# The SDK env-setup script exports CMAKE_TOOLCHAIN_FILE. CMake picks it up
# from the environment, but we pass it explicitly too so the toolchain
# path is obvious in build logs.
echo "==> CMake toolchain: $CMAKE_TOOLCHAIN_FILE"
echo "==> CC=$CC"
echo "==> sysroot=$SDKTARGETSYSROOT"

cd /work
rm -rf build-aarch64
# Release (not RelWithDebInfo) + post-build strip is the deployment shape
# we want on the U300 target: -O3, no debug info in the artefact,
# symbol table dropped. The strip step is structural — it lops ~1.9 MB
# off the binary without touching code or perf. If a developer needs
# debug info for a local trace, swap CMAKE_BUILD_TYPE back to
# RelWithDebInfo and skip the strip below.
cmake -S . -B build-aarch64 \
    -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DECNR_BUILD_LIVE=OFF

cmake --build build-aarch64 --target ecnr_bench -j$(nproc)

echo
echo "==> post-build strip --strip-all"
sz_pre=$(stat -c%s build-aarch64/ecnr_bench)
aarch64-poky-linux-strip --strip-all build-aarch64/ecnr_bench
sz_post=$(stat -c%s build-aarch64/ecnr_bench)
awk -v r=$sz_pre -v s=$sz_post 'BEGIN {
  printf "  pre-strip  : %12d B (%6.2f MB)\n", r, r/1048576
  printf "  post-strip : %12d B (%6.2f MB)\n", s, s/1048576
  printf "  delta      : %12d B (%6.2f MB)\n", r-s, (r-s)/1048576
}'
INNER
}

verify_script() {
  cat <<'INNER'
set -euo pipefail
cd /work
echo "==> artefact verification (expecting 'ELF 64-bit LSB executable, ARM aarch64'):"
for f in build-aarch64/ecnr_bench build-aarch64/libecnr_pipeline.a build-aarch64/libecnr_core.a build-aarch64/libecnr_rnnoise.a; do
  if [[ -e "$f" ]]; then
    printf "  %-50s " "$f"
    file -b "$f"
  fi
done

# Spot-check webrtc-apm too if the ExternalProject left it under build/
if [[ -d build-aarch64 ]]; then
  find build-aarch64 -name 'libwebrtc-audio-processing*' -type f 2>/dev/null | while read f; do
    printf "  %-50s " "$f"
    file -b "$f"
  done
fi
INNER
}

smoke_script() {
  # Executes the cross-built aarch64 ecnr_bench under qemu-aarch64-static
  # against the 30 s demo input. Confirms two things:
  #
  #   1. The binary is runnable (not broken at the dynamic-linker or
  #      runtime-symbol level) — closes PR #3's review item #2.
  #
  #   2. A reproducible baseline RTF / ERLE / runtime number for the
  #      aarch64 chain, suitable for tracking regressions across binary-
  #      size and perf experiments (Moves A and B in this work stream).
  #
  # Caveats:
  #   - qemu-user-static emulates aarch64 user-mode; reported wall-clock
  #     RTF includes emulation overhead and is NOT a real A55 number.
  #     Useful as a relative metric across builds, not as an absolute.
  #   - LD_LIBRARY_PATH points at the SDK target sysroot so the binary
  #     can resolve aarch64 libstdc++ / libsndfile / etc. against the
  #     glibc compiled into the SDK.
  cat <<'INNER'
set -euo pipefail
cd /work
if [[ ! -x build-aarch64/ecnr_bench ]]; then
  echo "build-aarch64/ecnr_bench not found — run --bench first" >&2
  exit 1
fi
export QEMU_LD_PREFIX="$SDKTARGETSYSROOT"
echo "==> file build-aarch64/ecnr_bench"
file build-aarch64/ecnr_bench
echo
echo "==> binary footprint (as-built vs strip --strip-all):"
sz_raw=$(stat -c%s build-aarch64/ecnr_bench)
cp build-aarch64/ecnr_bench /tmp/ecnr_bench_stripped
aarch64-poky-linux-strip --strip-all /tmp/ecnr_bench_stripped
sz_strip=$(stat -c%s /tmp/ecnr_bench_stripped)
awk -v r=$sz_raw -v s=$sz_strip 'BEGIN {
  printf "  as-built    : %12d B (%6.2f MB)\n", r, r/1048576
  printf "  --strip-all : %12d B (%6.2f MB)\n", s, s/1048576
  printf "  delta       : %12d B (%6.2f MB)\n", r-s, (r-s)/1048576
}'
rm -f /tmp/ecnr_bench_stripped
echo
echo "==> aarch64 chain smoke under qemu-aarch64-static (30 s demo, 16 kHz):"
echo "    (bench prints its own RTF — that's the meaningful number; qemu wall-clock"
echo "     is RTF × emulation overhead, useful only as a relative baseline.)"
echo
qemu-aarch64-static build-aarch64/ecnr_bench \
    --mic reference/synth/demo_30s_mic.wav \
    --ref reference/synth/demo_30s_ref.wav \
    --out /tmp/smoke_out.wav \
    --bypass-beamformer
INNER
}

case "$mode" in
  build)
    build_image
    run_in_container "$(cross_build_deps_script)"
    run_in_container "$(cross_build_bench_script)"
    run_in_container "$(verify_script)"
    ;;
  --image)
    build_image
    ;;
  --deps)
    build_image
    run_in_container "$(cross_build_deps_script)"
    ;;
  --bench)
    run_in_container "$(cross_build_bench_script)"
    ;;
  --verify)
    run_in_container "$(verify_script)"
    ;;
  --smoke)
    build_image
    run_in_container "$(smoke_script)"
    ;;
  --shell)
    build_image
    docker run --rm -it \
      --platform=linux/amd64 \
      --name "$CONTAINER_NAME" \
      -v "$REPO_ROOT":/work \
      -v "ecnr-cross-yocto-sdk-extras:/opt/poky-extras" \
      "$IMAGE_NAME"
    ;;
  *)
    echo "usage: $0 [build|--image|--deps|--bench|--verify|--smoke|--shell]"
    exit 2
    ;;
esac

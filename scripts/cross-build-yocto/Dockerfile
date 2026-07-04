# Cross-compile environment for the U300 (Yocto / aarch64) target.
#
# Builds a Linux/amd64 image that hosts the Yocto pre-built reference SDK
# (Poky, scarthgap 5.0.7) targeting cortexa57-qemuarm64. This is a STAND-IN
# for the U300 vendor SDK: same toolchain shape (environment-setup-*
# script, /opt/poky/<ver>/sysroots/, aarch64-poky-linux- prefix), but a
# generic reference sysroot. The exact CPU tuning (cortexa57) differs
# from U300's cortexa55 — A57 binaries run on A55 (forward-compatible at
# the ARMv8.0-A ISA level), so this is fine for build-mechanics validation.
#
# Swapping in the real U300 SDK: change the SDK_URL / SDK_INSTALL_PATH
# build args, or mount the vendor SDK installer .sh into /tmp/sdk-installer
# and re-run the install step. The CMake toolchain file (cmake/) and the
# Meson cross-file (meson/) reference the env-setup script by glob, so
# they pick up whatever the SDK installed.
#
# Host arch note: Yocto Project only publishes x86_64-host SDKs as pre-
# built. On Apple Silicon hosts this runs under Rosetta — slower but
# compatible. We pin --platform=linux/amd64 explicitly so Docker doesn't
# pick the wrong base image on aarch64 hosts.

FROM --platform=linux/amd64 debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive

# Build prerequisites — anything the SDK or our cross-builds need.
# autotools + meson + ninja + cmake covers the three build systems in
# play (rnnoise upstream is autotools; webrtc-apm is meson; ecnr_bench
# is cmake). pkg-config + libtool round out the autotools chain.
# python3-pkg-resources is for Meson; xz-utils for the SDK installer.
# apt over https with retries: observed 2026-07-04 that Docker Desktop's
# amd64-emulation network path intermittently kills large plain-http
# fetches from deb.debian.org (its internal proxy dropped the ~8 MB
# Packages index at KB/s rates → exit 100). https rides a different
# proxy path and Acquire::Retries absorbs the flakes.
RUN sed -i 's|http://deb.debian.org|https://deb.debian.org|g' \
        /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        autoconf automake libtool pkg-config \
        cmake meson ninja-build \
        build-essential file git curl ca-certificates xz-utils \
        python3 python3-pip python3-pkg-resources \
        qemu-user-static \
    && rm -rf /var/lib/apt/lists/*

# Download + install the Yocto reference SDK. The installer is ~300 MB
# and unpacks to ~1.2 GB; pinning the URL keeps the layer cacheable.
# Poky's reference SDKs for cortexa57-qemuarm64 are published in three
# variants: ext (extensible, devtool-enabled), sato (with GTK/X11 base),
# and sato-ext. A "regular minimal" non-extensible doesn't exist pre-built
# at downloads.yoctoproject.org, so we use the regular Sato SDK — the
# extra X11/GTK content in the sysroot is harmless for our purposes and
# may carry libsndfile/abseil pre-built (saving us cross-build work).
ARG YOCTO_VERSION=5.0.7
ARG SDK_TUNING=cortexa57
ARG SDK_MACHINE=qemuarm64
ARG SDK_FILENAME=poky-glibc-x86_64-core-image-sato-${SDK_TUNING}-${SDK_MACHINE}-toolchain-${YOCTO_VERSION}.sh
ARG SDK_URL=https://downloads.yoctoproject.org/releases/yocto/yocto-${YOCTO_VERSION}/toolchain/x86_64/${SDK_FILENAME}
ENV SDK_INSTALL_PATH=/opt/poky/${YOCTO_VERSION}

RUN mkdir -p /tmp/sdk \
    && curl -fsSL "${SDK_URL}" -o "/tmp/sdk/install.sh" \
    && chmod +x "/tmp/sdk/install.sh" \
    && "/tmp/sdk/install.sh" -y -d "${SDK_INSTALL_PATH}" \
    && rm -rf /tmp/sdk

# The env-setup script name encodes the tuning (e.g.,
# environment-setup-cortexa57-poky-linux). Capture its absolute path so
# subsequent layers and the user can `source $SDK_ENV` to enter the
# cross-compile shell. find used at build time because the suffix is
# tuning-dependent.
RUN ln -sf $(ls ${SDK_INSTALL_PATH}/environment-setup-*-poky-linux | head -1) /opt/poky/environment-setup-current
ENV SDK_ENV=/opt/poky/environment-setup-current

# Workaround for a Scarthgap-era Yocto SDK bug: the auto-generated meson
# cross-file leaves [host_machine] cpu_family = 'x86_64' (the build host)
# rather than the target ('aarch64'). Anything that gates SIMD/NEON code
# on `host_machine.cpu_family() == 'aarch64'` (e.g. vendor/webrtc-audio-
# processing/meson.build:140) silently misses our build and then fails
# the `.require()` check. We can't override via a chained --cross-file
# because Yocto's meson wrapper script appends the SDK cross-file LAST,
# making it the winner on conflicting keys. Easiest robust fix: patch
# the SDK cross-file in place. This Dockerfile layer makes the fix part
# of the image, so it's reproducible and visible in `docker history`.
#
# If the U300 vendor SDK has the correct cpu_family in its cross-file,
# this sed is a no-op (the regex won't match) — leave it in place.
RUN find ${SDK_INSTALL_PATH}/sysroots/x86_64-pokysdk-linux/usr/share/meson/ \
        -name 'aarch64-*-meson.cross' -print -exec \
        sed -i -e "s/^cpu_family = 'x86_64'/cpu_family = 'aarch64'/" \
               -e "s/^cpu = 'x86_64'/cpu = 'cortex-a55'/" {} \; \
    && find ${SDK_INSTALL_PATH}/sysroots/x86_64-pokysdk-linux/usr/share/meson/ \
        -name 'aarch64-*-meson.cross' -exec grep -E 'cpu_family|^cpu ' {} \;

# Working directory mirrors the host bind-mount target.
WORKDIR /work

# Wrapper that sources the SDK env before exec'ing whatever args are
# passed. Used by both interactive (`-it`) and one-shot (`-c '...'`)
# invocations, so the cross-compile environment is always present.
# Quoting is important: `exec "$@"` forwards argv intact, including
# `-c "..."` strings that contain spaces.
RUN cat > /usr/local/bin/cross-entry <<'EOF' && chmod +x /usr/local/bin/cross-entry
#!/usr/bin/env bash
set -eu
. "$SDK_ENV"
if [[ $# -eq 0 ]]; then
  exec /bin/bash
fi
exec /bin/bash "$@"
EOF

ENTRYPOINT ["/usr/local/bin/cross-entry"]

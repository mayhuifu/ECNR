# Cross-compile environment for ECNR on aarch64 (Yocto / U300)

This directory bootstraps a local cross-compile environment for the U300
target (aarch64, Yocto-based Linux), closing **assumption A7** of
[ADR-0001](../../docs/adr/0001-hybrid-aec-architecture-review.md): _"the
existing macOS host build is reproducible for the A55 target"_.

It is **not** a production CI environment. The point is to validate that
the project's build mechanics (CMake + Meson + autotools across vendored
deps) cross-compile cleanly for `aarch64-poky-linux`, using the same
SDK shape the U300 vendor will eventually ship.

See [`REPORT.md`](REPORT.md) for the spike outcome and what to do next.

## What this is

A Docker container that hosts the Yocto Poky reference SDK targeting
`cortexa57-qemuarm64`, plus a driver script that runs the project's
cross-compile chain inside it. The reference SDK is a **stand-in** for
the U300 vendor SDK; the spike's deliverable is the toolchain plumbing
(Dockerfile, build script, CMake / Meson cross-compile glue), not the
specific binary it produces.

```
scripts/cross-build-yocto/
├── Dockerfile          # linux/amd64 base + Yocto SDK install (~5 GB total)
├── build.sh            # driver: --image | --deps | --bench | --verify | --shell
├── README.md           # ← this file
└── REPORT.md           # spike outcome + measured findings
```

## Why a container

The Yocto Project only publishes pre-built reference SDKs for **x86_64
Linux** hosts. On Apple Silicon (and on any non-Linux host) the SDK
installer must run in a Linux container. We use Docker Desktop's
`linux/amd64` platform (Rosetta on Apple Silicon, native on Intel).

## Why Sato, not minimal

`core-image-minimal` has no pre-built SDK for cortexa57-qemuarm64 — only
the `-ext` extensible variant. The regular Sato SDK ships a more complete
sysroot (libsndfile, speexdsp, alsa already present), so we use it. The
extra X11 / GTK content in the sysroot is harmless for our purposes.

When the U300 vendor SDK lands, this choice becomes moot — the vendor
sysroot ships whatever the BSP carries.

## Usage

```sh
# Full chain (image build + deps + bench + verify). First run downloads
# ~765 MB SDK and unpacks ~5 GB; subsequent runs use Docker layer cache.
scripts/cross-build-yocto/build.sh

# Per-step:
scripts/cross-build-yocto/build.sh --image    # build container + install SDK
scripts/cross-build-yocto/build.sh --deps     # cross-build missing libs
scripts/cross-build-yocto/build.sh --bench    # cross-build ecnr_bench
scripts/cross-build-yocto/build.sh --verify   # `file` the artefacts
scripts/cross-build-yocto/build.sh --shell    # interactive shell w/ SDK env
```

Cross-built artefacts land at `build-aarch64/` in the host repo (covered
by `.gitignore`'s `build-*/` rule).

## Swapping in the U300 vendor SDK

When the BSP team supplies the U300 SDK installer (typically a
`<sdk-name>.sh` shell script with embedded payload):

1. Drop the installer at a path the Dockerfile can reach (e.g. copy it
   into `scripts/cross-build-yocto/` and add a `COPY` line, or mount it
   into the container at run time).
2. Replace the `curl ... | install.sh` block in the Dockerfile with the
   vendor installer invocation.
3. Update `SDK_INSTALL_PATH` if the vendor's default differs from
   `/opt/poky/<version>/`.
4. Verify the env-setup script still has a `*-poky-linux` suffix (or
   adjust the symlink target).

Everything else — `build.sh`, the CMake + Meson glue in
`cmake/BuildWebRTCAPM.cmake`, the placeholder geometry — stays the same.

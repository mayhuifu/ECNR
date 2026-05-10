# third_party/

Build-time dependencies that are **not** core algorithm code (those live in `vendor/`). Examples: `googletest` for unit tests, optional bundled `libsndfile`.

Currently:

- **`miniaudio/`** — single-header cross-platform audio library, version 0.11.21. Used by `ecnr_live` (Phase 0.6) for mic capture + speaker playback. Public-domain / MIT-0 license; vendored so builds don't depend on a network during compile.
- **`googletest`** — pulled in via CMake `FetchContent` from `CMakeLists.txt`; nothing committed here directly.
- **`libsndfile`** — sourced from the system (`brew install libsndfile` / `apt install libsndfile1-dev`). If unavailable, document a vendored fallback here.

This directory exists to keep build infrastructure separate from algorithm sources, so license and provenance review for `vendor/` stays focused.

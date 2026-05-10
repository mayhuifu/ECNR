# third_party/

Build-time dependencies that are **not** core algorithm code (those live in `vendor/`). Examples: `googletest` for unit tests, optional bundled `libsndfile`.

Phase 0 expects:

- `googletest` — pulled in via CMake `FetchContent` from `CMakeLists.txt`. Nothing committed here directly.
- `libsndfile` — preferred from system (`brew install libsndfile` / `apt install libsndfile1-dev`). If unavailable, document a vendored fallback here.

This directory exists to keep build infrastructure separate from algorithm sources, so license and provenance review for `vendor/` stays focused.

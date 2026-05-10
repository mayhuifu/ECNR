cmake_minimum_required(VERSION 3.20)
project(ecnr LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Phase 0 build:
#   Scaffold + stub backends. Real WebRTC AEC3 + RNNoise wiring lands in Phase
#   0.5 behind the same AecChain interface. Only system deps for now: a C++17
#   compiler, libsndfile, and (auto-fetched) googletest.
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)

# --- libsndfile -----------------------------------------------------------------
# Prefer pkg-config (works for both Homebrew and most Linux distros).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

# --- webrtc-audio-processing (vendored, built via Meson + ExternalProject) -----
# Produces the imported target `webrtc::apm`. Downstream code (Phase 0.5
# Task 6) will link against this; nothing in the pipeline uses it yet.
include(cmake/BuildWebRTCAPM.cmake)

# --- googletest -----------------------------------------------------------------
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# --- ecnr_core (frame, ring buffer) --------------------------------------------
add_library(ecnr_core STATIC
  src/core/ring_buffer.cc
)
target_include_directories(ecnr_core PUBLIC src)

# --- ecnr_pipeline (AEC chain) -------------------------------------------------
add_library(ecnr_pipeline STATIC
  src/pipeline/aec_chain.cc
)
target_link_libraries(ecnr_pipeline PUBLIC ecnr_core)

# --- ecnr_hal (file I/O via libsndfile) ----------------------------------------
add_library(ecnr_hal STATIC
  src/hal/file_io.cc
)
target_link_libraries(ecnr_hal PUBLIC ecnr_core PkgConfig::SNDFILE)

# --- ecnr_bench (offline harness) ----------------------------------------------
add_executable(ecnr_bench src/bench/main.cc)
target_link_libraries(ecnr_bench PRIVATE ecnr_pipeline ecnr_hal)

# --- ecnr_live (host loopback E2E via miniaudio) -------------------------------
# miniaudio is a single-header library — we vendor the pinned header at
# third_party/miniaudio/. Linker flags below cover macOS CoreAudio; on Linux
# we link ALSA + pthread + dl + m.
add_executable(ecnr_live src/live/live_loopback.cc)
target_link_libraries(ecnr_live PRIVATE ecnr_pipeline ecnr_hal)
target_include_directories(ecnr_live PRIVATE third_party/miniaudio)
if(APPLE)
  target_link_libraries(ecnr_live PRIVATE
    "-framework CoreFoundation"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AudioUnit"
  )
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(ecnr_live PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
  pkg_check_modules(ALSA alsa)
  if(ALSA_FOUND)
    target_link_libraries(ecnr_live PRIVATE PkgConfig::ALSA)
  endif()
endif()

# --- tests ---------------------------------------------------------------------
enable_testing()
add_executable(aec_chain_test src/tests/aec_chain_test.cc)
target_link_libraries(aec_chain_test PRIVATE ecnr_pipeline GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(aec_chain_test)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
message(STATUS "ecnr: Phase 0 build (stub backends; Phase 0.5 swaps in AEC3 + RNNoise)")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  sndfile      : ${SNDFILE_VERSION}")

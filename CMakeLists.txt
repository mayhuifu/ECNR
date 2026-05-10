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

# Drives the Meson build of vendor/webrtc-audio-processing as a CMake
# ExternalProject. Produces an imported target `webrtc::apm` that downstream
# CMake targets can `target_link_libraries(... PRIVATE webrtc::apm)`.

include(ExternalProject)

set(WEBRTC_APM_SOURCE_DIR  "${CMAKE_SOURCE_DIR}/vendor/webrtc-audio-processing")
set(WEBRTC_APM_BUILD_DIR   "${CMAKE_BINARY_DIR}/webrtc-apm-build")
set(WEBRTC_APM_INSTALL_DIR "${CMAKE_BINARY_DIR}/webrtc-apm-install")

if(NOT EXISTS "${WEBRTC_APM_SOURCE_DIR}/meson.build")
  message(FATAL_ERROR
    "vendor/webrtc-audio-processing not found. Run scripts/fetch-vendor.sh first.")
endif()

set(WEBRTC_APM_LIB_NAME "libwebrtc-audio-processing-2.a")
set(WEBRTC_APM_LIB_PATH "${WEBRTC_APM_INSTALL_DIR}/lib/${WEBRTC_APM_LIB_NAME}")
set(WEBRTC_APM_INCLUDE_DIR "${WEBRTC_APM_INSTALL_DIR}/include/webrtc-audio-processing-2")

# Upstream meson.build does not link CoreFoundation on darwin even though the
# WEBRTC_MAC code path in rtc_base/logging.cc references CFBundleGetMainBundle
# and friends. The static lib itself is fine, but the bundled `run-offline`
# example fails to link, which aborts the meson build before install. We work
# around it by injecting -framework CoreFoundation into the link args; this is
# only needed at example-link time and is harmless for our static lib output.
set(_WEBRTC_APM_EXTRA_MESON_ARGS "")
if(APPLE)
  list(APPEND _WEBRTC_APM_EXTRA_MESON_ARGS
    "-Dcpp_link_args=-framework CoreFoundation"
    "-Dc_link_args=-framework CoreFoundation"
  )
endif()

ExternalProject_Add(webrtc_apm_external
  SOURCE_DIR "${WEBRTC_APM_SOURCE_DIR}"
  BINARY_DIR "${WEBRTC_APM_BUILD_DIR}"
  CONFIGURE_COMMAND meson setup
    --prefix=${WEBRTC_APM_INSTALL_DIR}
    --buildtype=release
    --default-library=static
    -Dneon=disabled
    ${_WEBRTC_APM_EXTRA_MESON_ARGS}
    "${WEBRTC_APM_BUILD_DIR}"
    "${WEBRTC_APM_SOURCE_DIR}"
  BUILD_COMMAND meson compile -C "${WEBRTC_APM_BUILD_DIR}"
  INSTALL_COMMAND meson install -C "${WEBRTC_APM_BUILD_DIR}"
  BUILD_BYPRODUCTS "${WEBRTC_APM_LIB_PATH}"
)

# Create the include dir at configure time so the IMPORTED target's
# INTERFACE_INCLUDE_DIRECTORIES check passes before the external build runs.
file(MAKE_DIRECTORY "${WEBRTC_APM_INCLUDE_DIR}")

add_library(webrtc::apm STATIC IMPORTED GLOBAL)
add_dependencies(webrtc::apm webrtc_apm_external)
set_target_properties(webrtc::apm PROPERTIES
  IMPORTED_LOCATION "${WEBRTC_APM_LIB_PATH}"
  INTERFACE_INCLUDE_DIRECTORIES "${WEBRTC_APM_INCLUDE_DIR}"
)

# Abseil is a transitive dep of WebRTC APM. Find via pkg-config (installed via
# brew/apt). The two roots below pull in the closure of absl symbols the APM
# code references.
pkg_check_modules(ABSL REQUIRED IMPORTED_TARGET absl_strings absl_synchronization)
target_link_libraries(webrtc::apm INTERFACE PkgConfig::ABSL)

# rtc_base/logging.cc on darwin references CFBundleGetMainBundle and friends
# (WEBRTC_MAC code path). Anyone linking the static archive must pull in
# CoreFoundation or the link fails with undefined CF* symbols.
if(APPLE)
  target_link_libraries(webrtc::apm INTERFACE "-framework CoreFoundation")
endif()

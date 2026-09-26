# A fully independent CMake project, not add_subdirectory()'d into the
# root build - see cmake/toolchain-mps2an385.cmake's own top comment for
# why (the same reasoning web/CMakeLists.txt gives for its own wasm32
# cross-compile). Configured via its own "rp2040" preset
# (CMakePresets.json, next to this file).
#
# Unlike mps2an385, boot2/clock bring-up/the linker script all come from
# the vendored third_party/pico-sdk (BSD-3-Clause) rather than being
# hand-written here - pico_sdk_init() (from pico-sdk's own
# pico_sdk_init.cmake, included below before project()) does the real
# work; this file only adds est/estrp2040 on top of it, the same shape
# mps2an385/CMakeLists.txt already establishes for a from-scratch target.
cmake_minimum_required(VERSION 4.4)

set(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico-sdk" CACHE PATH "")
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(est_rp2040 LANGUAGES C CXX ASM)

pico_sdk_init()

# CMAKE_CXX_SCAN_FOR_MODULES/CMAKE_CXX_MODULE_STD are directory-scope
# settings that apply to every C++ target configured from this point
# forward - deliberately set *after* pico_sdk_init() rather than before:
# pico-sdk's own internal targets (added by pico_sdk_init()'s
# add_subdirectory() above) are plain C/C++, not modules-aware, and some
# don't explicitly request a high enough C++ standard for the synthetic
# `import std;` target CMake generates once module scanning is on -
# setting these only for what's added below (est/estrp2040/the example
# itself) keeps pico-sdk's own targets unaffected.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

set(EST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# --- est itself, cross-compiled for armv6m-none-eabi - the real
# est/CMakeLists.txt, add_subdirectory()'d directly (issue #128), same as
# mps2an385/CMakeLists.txt's own identical block.
set(EST_BUILD_TESTS OFF)
add_subdirectory("${EST_ROOT}/est" "${CMAKE_CURRENT_BINARY_DIR}/est")

# --- estrp2040: this build's own platform::interface backend, built on
# pico-sdk's own hardware_timer/hardware_uart/hardware_gpio/hardware_irq
# library targets (real clock/reset bring-up already done by pico-sdk's
# own runtime_init(), run automatically before main()) rather than
# hand-poked MMIO registers - see platform_rp2040.cppm's own top comment.
add_library(estrp2040 STATIC)
target_sources(estrp2040
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${EST_ROOT}/estrp2040/src"
    FILES
      "${EST_ROOT}/estrp2040/src/platform_rp2040.cppm"
)
target_compile_features(estrp2040 PUBLIC cxx_std_23)
target_link_libraries(estrp2040 PUBLIC est hardware_timer hardware_uart hardware_gpio hardware_irq)

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

# est genuinely throws (future<T>'s own exception propagation,
# est::check(), platform_rp2040.cppm's own try/catch in
# pump_task_loop()) - pico-sdk's own C++ targets default to
# PICO_CXX_ENABLE_EXCEPTIONS=0/PICO_CXX_ENABLE_RTTI=0 (a real embedded-
# systems-friendly default for typical pico-sdk users who don't need
# either), applied via its own pico_cxx_options INTERFACE library, whose
# -fno-exceptions/-fno-rtti wins over anything the toolchain file itself
# tries to set (cmake/toolchain-rp2040.cmake's own comment has the
# empirical confirmation) - this is the one sanctioned place that
# actually overrides it, read by pico_cxx_options/CMakeLists.txt before
# any C++ target gets configured.
set(PICO_CXX_ENABLE_EXCEPTIONS 1 CACHE BOOL "")
set(PICO_CXX_ENABLE_RTTI 1 CACHE BOOL "")

# pico-sdk's own default thread-local implementation ("per_thread",
# pico_thread_local's own real per-core TLS emulation) defines
# `_set_tls`/`__aeabi_read_tp` as *strong* symbols - which collide
# outright with this ARM ATfE sysroot's own picolibc, which (unlike a
# typical arm-none-eabi-gcc+newlib toolchain pico-sdk was originally
# tested against) already bundles a real, working implementation of the
# exact same low-level ARM TLS primitives ("duplicate symbol: _set_tls"/
# "__aeabi_read_tp", confirmed empirically). "global" mode's own
# __aeabi_read_tp is declared `__weak` instead, so picolibc's own strong
# definition wins with no conflict - and "global" (thread_local behaves
# like an ordinary shared global) is the semantically correct choice
# here regardless of the symbol-collision angle: this backend is core0
# only (docs/PLAN.md's own entry), so there's no second execution
# context for real per-thread storage to ever distinguish.
set(PICO_DEFAULT_THREAD_LOCAL_IMPL "global" CACHE STRING "")

# "global" mode alone isn't quite enough: its own __aeabi_read_tp is
# __weak (letting picolibc's own strong definition win, no conflict),
# but its _set_tls is still unconditionally strong regardless of mode
# (thread_local.c's own PICO_THREAD_LOCAL_PROVIDE_SET_TLS, default 1,
# gates that one function specifically - pico/thread_local.h's own doc
# comment: "Whether pico_thread_local should provide its own
# implementation of _set_tls"). Disabling it here defers to picolibc's
# own working implementation instead, the same as __aeabi_read_tp's own
# weak fallback already does - not a correctness compromise: "global"
# mode's own _set_tls body is a no-op in spirit anyway (there's no real
# per-context TLS block to switch to when every thread_local variable is
# just a shared global). Added once picolibc's own "duplicate symbol:
# _set_tls" confirmed this is needed too, not just __aeabi_read_tp.
add_compile_definitions(PICO_THREAD_LOCAL_PROVIDE_SET_TLS=0)

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
# Also provides estrp2040::Serial (serial.cppm, a module partition of the
# same `estrp2040` module), the USB CDC class - built on pico-sdk's own
# vendored tinyusb_device; PUBLIC include of this module's own src/ dir
# is what makes tusb_config.h visible both to this compile and to
# whatever executable ultimately links tinyusb_device's own sources in
# (see tusb_config.h's own top comment for why this module, not the
# consuming example, owns it).
# cxa_thread_atexit_shim.c: this sysroot's own picolibc never implements
# __cxa_thread_atexit at all (confirmed empirically, that file's own top
# comment has the full story) - needed the moment anything reachable
# calls est::spawn(), which pulls in est::detail::
# spawn_exception_hook_storage's own non-trivially-destructible
# thread_local. A plain C source, not part of the FILE_SET CXX_MODULES
# list above.
add_library(estrp2040 STATIC)
target_sources(estrp2040
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${EST_ROOT}/estrp2040/src"
    FILES
      "${EST_ROOT}/estrp2040/src/platform_rp2040.cppm"
      "${EST_ROOT}/estrp2040/src/serial.cppm"
  PRIVATE
      "${EST_ROOT}/estrp2040/src/cxa_thread_atexit_shim.c"
)
target_compile_features(estrp2040 PUBLIC cxx_std_23)
target_include_directories(estrp2040 PUBLIC "${EST_ROOT}/estrp2040/src")
target_link_libraries(estrp2040 PUBLIC
  est hardware_timer hardware_uart hardware_gpio hardware_irq tinyusb_device)

# --- app_demo: the project-supplied est::future<void> app() a separate,
# future project would provide instead - a minimal USB CDC echo, proving
# the whole pipeline end-to-end the same way every other backend in this
# repo has a real, buildable example rather than a bare skeleton.
add_library(app_demo STATIC)
target_sources(app_demo
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/src/app_demo.cppm"
)
target_compile_features(app_demo PUBLIC cxx_std_23)
target_link_libraries(app_demo PUBLIC est estrp2040)

# --- the actual firmware image: no hand-written boot2/startup/link.ld
# needed at all (unlike mps2an385) - pico-sdk's own pico_standard_link
# (auto-linked into every pico-sdk executable target by pico_sdk_init()'s
# own add_executable() override) already supplies a working default
# linker script/vector table/boot2, confirmed against a real memory
# layout (docs/PLAN.md's own entry for this backend has the readelf
# output).
add_executable(rp2040_demo src/main.cpp src/usb_descriptors.c)
target_link_libraries(rp2040_demo PRIVATE est estrp2040 app_demo pico_stdlib)
pico_add_extra_outputs(rp2040_demo)

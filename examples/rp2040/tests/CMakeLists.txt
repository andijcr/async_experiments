# Real Catch2 v3.7.1, patched at build time for the same bare-metal gaps
# mps2an385/tests/CMakeLists.txt's own identical block already documents
# (filesystem/monotonic clock/std::random_device - the patch itself is a
# plain copy of that file's ../third_party/catch2-baremetal.patch, kept
# here rather than cross-referenced: this project re-declares its own
# sources throughout, matching web/CMakeLists.txt's/mps2an385's own
# "independent CMake project, no reaching into a sibling project's tree"
# convention).
include(FetchContent)
find_package(Git REQUIRED)

FetchContent_Declare(
  Catch2Baremetal
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  PATCH_COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths -p1
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/catch2-baremetal.patch"
  UPDATE_DISCONNECTED TRUE
  SOURCE_SUBDIR does-not-exist
)
FetchContent_MakeAvailable(Catch2Baremetal)

include("${catch2baremetal_SOURCE_DIR}/CMake/CatchConfigOptions.cmake")

set(CATCH_CONFIG_NO_POSIX_SIGNALS ON CACHE BOOL "" FORCE)
set(CATCH_CONFIG_NOSTDOUT ON CACHE BOOL "" FORCE)
set(CATCH_CONFIG_NO_WCHAR ON CACHE BOOL "" FORCE)

configure_file(
  "${catch2baremetal_SOURCE_DIR}/src/catch2/catch_user_config.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated-includes/catch2/catch_user_config.hpp"
)

file(GLOB_RECURSE CATCH2_BAREMETAL_SOURCES CONFIGURE_DEPENDS
  "${catch2baremetal_SOURCE_DIR}/src/catch2/*.cpp")

add_library(Catch2Baremetal STATIC ${CATCH2_BAREMETAL_SOURCES})
target_include_directories(Catch2Baremetal SYSTEM PUBLIC
  "${catch2baremetal_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated-includes")
target_compile_features(Catch2Baremetal PUBLIC cxx_std_20)
target_compile_definitions(Catch2Baremetal PUBLIC
  CATCH_BAREMETAL_NO_MONOTONIC_CLOCK
  CATCH_BAREMETAL_NO_RANDOM_DEVICE
  CATCH_BAREMETAL_NO_FILESYSTEM)
target_compile_options(Catch2Baremetal PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

# --- est's own real test suite, reused unmodified from est/tests/ - same
# shape as mps2an385/tests/CMakeLists.txt's own identical block (only
# test_main.cpp differs - installs estrp2040::platform_rp2040 instead of
# estmsp::platform_mps2an385).
#
# Unlike mps2an385/tests/, no board-specific ISR-driven spsc_ring test is
# added here yet - this first pass is the Renode CI feasibility spike
# itself (docs/PLAN.md's own entry), not full ISR-coverage parity; a real
# UART0-IRQ-driven analog can follow once the base boot/test signal chain
# is proven.
set(EST_TESTS_DIR "${EST_ROOT}/est/tests")
# usb_descriptors.c: estrp2040 links tinyusb_device unconditionally (for
# estrp2040::Serial), which pulls in usbd.c's own calls to
# tud_descriptor_device_cb()/tud_descriptor_configuration_cb()/
# tud_descriptor_string_cb() regardless of whether this specific binary
# ever touches Serial - the same descriptors src/usb_descriptors.c
# already supplies for rp2040_demo, reused unmodified here rather than a
# second copy (this test suite has no product identity of its own to
# supply).
add_executable(est_rp2040_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/usb_descriptors.c"
  "${EST_TESTS_DIR}/platform_tests.cpp"
  "${EST_TESTS_DIR}/check_tests.cpp"
  "${EST_TESTS_DIR}/intrusive_list_tests.cpp"
  "${EST_TESTS_DIR}/mutex_tests.cpp"
  "${EST_TESTS_DIR}/event_tests.cpp"
  # external_event_tests.cpp excluded: a real, narrow, unresolved Clang
  # miscompilation on THIS target specifically (armv6m_soft_nofp_exn_rtti
  # - not seen on hosted/wasm32/mps2an385's own armv7m_soft_nofp_exn_rtti
  # sysroot slice of the identical toolchain release). Name lookup on
  # `bridge.poll()` resolves to detail::external_source::poll() (private,
  # inaccessible) instead of external_event<T>'s own public redeclaration
  # of the same virtual - a real compiler bug candidate, not a design
  # issue in external_event.cppm itself, and out of scope for this spike
  # (docs/PLAN.md's own entry) to chase further; tracked as follow-up
  # work, not silently dropped.
  "${EST_TESTS_DIR}/spsc_ring_tests.cpp"
  "${EST_TESTS_DIR}/timer_tests.cpp"
  "${EST_TESTS_DIR}/loop_tests.cpp"
  "${EST_TESTS_DIR}/jitter_tests.cpp"
  "${EST_TESTS_DIR}/timer_periodic_tests.cpp"
  "${EST_TESTS_DIR}/future_tests.cpp"
  "${EST_TESTS_DIR}/scope_exit_tests.cpp"
  "${EST_TESTS_DIR}/shared_ptr_tests.cpp"
  "${EST_TESTS_DIR}/when_all_tests.cpp"
  "${EST_TESTS_DIR}/when_any_tests.cpp"
  "${EST_TESTS_DIR}/when_any_succeeds_tests.cpp"
  "${EST_TESTS_DIR}/stop_token_tests.cpp"
  "${EST_TESTS_DIR}/with_stop_tests.cpp"
  "${EST_TESTS_DIR}/with_timeout_tests.cpp"
  "${EST_TESTS_DIR}/spawn_tests.cpp"
)
# pico_stdlib is required here (pico_atomic's __atomic_* intrinsics,
# pico_sync's __mutex_array_start/end, hardware_irq's __unhandled_user_irq
# - all genuinely needed once a real, larger binary like this one links) -
# see toolchain-rp2040.cmake's own --allow-multiple-definition comment for
# the one real conflict pico_stdlib brings along with it
# (pico_clib_interface's own cxa_guard.c) and why it's handled at the
# linker-flag level instead of by dropping this dependency.
target_link_libraries(est_rp2040_tests PRIVATE est estrp2040 Catch2Baremetal pico_stdlib)
target_compile_options(est_rp2040_tests PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)

# pico-sdk's own default PICO_STACK_SIZE (0x800 - pico/platform.h's own
# doc comment) is genuinely too small for this specific binary: a real
# crash was observed under Renode (a CPU abort - "Trying to execute code
# outside RAM or ROM" - preceded by writes to obviously garbage
# addresses, the classic signature of a stack overflow corrupting a
# return address until control flow runs off into the weeds) that
# stopped reproducing once this was raised. Catch2's own call chains plus
# est's real exception machinery need more than mps2an385's own
# already-doubled __stack_size (0x1000, examples/multicolor_larson_scanner/
# mps2an385/src/link.ld) - RP2040's 264KB SRAM has ample room to spare.
# crt0.S reads this directly (`.equ StackSize, PICO_STACK_SIZE`), so a
# per-target compile definition is enough - no linker script change
# needed. Capped at 0x1000: pico-sdk's own default linker script
# (memmap_default.ld) places the stack in one of RP2040's two dedicated
# 4KB SCRATCH_X/SCRATCH_Y SRAM banks, not the main 256KB pool - confirmed
# empirically (0x2000 overflows SCRATCH_Y outright, exactly by 4096
# bytes) - so 0x1000 is the most this specific region can hold regardless
# of how much of the 264KB total SRAM is otherwise free. Matches
# mps2an385's own __stack_size exactly (examples/multicolor_larson_scanner/
# mps2an385/src/link.ld) - the same doubling already proven sufficient for
# this identical Catch2+est test suite there.
target_compile_definitions(est_rp2040_tests PRIVATE PICO_STACK_SIZE=0x1000)

pico_add_extra_outputs(est_rp2040_tests)

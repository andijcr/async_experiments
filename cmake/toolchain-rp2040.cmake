# CMake toolchain file for examples/rp2040's real Raspberry Pi Pico
# (RP2040) build - a sibling to toolchain-mps2an385.cmake, but a thin
# wrapper around pico-sdk's own official Clang toolchain file
# (third_party/pico-sdk/cmake/preload/toolchains/
# pico_arm_cortex_m0plus_clang.cmake) rather than a from-scratch one:
# pico-sdk supplies boot2/clock-bring-up/hardware headers itself, and its
# own toolchain file already knows how to drive Clang for armv6m-none-eabi
# - this file only bridges the gaps between that file's own assumptions
# and this image's pinned Clang ${LLVM_VERSION} (docker/Dockerfile),
# empirically found and fixed one at a time against a real build (not
# theorized): see each block's own comment below for the underlying cause.
#
# The consuming CMakeLists.txt must `include(.../pico_sdk_init.cmake)`
# (setting PICO_SDK_PATH) *before* `project()`, exactly as any other
# pico-sdk-based project does - this file reads ${PICO_SDK_PATH}, it
# doesn't set it.
#
# Applied via the "rp2040" preset in examples/rp2040/CMakePresets.json -
# that subdirectory is a fully separate CMake project (its own project(),
# its own presets file), not add_subdirectory()'d into the root build, for
# the same reason toolchain-mps2an385.cmake's own top comment gives.

# Same reasoning as toolchain-mps2an385.cmake's own identical line: a
# full EXECUTABLE-type ABI-detection try_compile needs a working crt0/
# linker script, which only exists once a target actually links against
# pico-sdk's own `pico_standard_link` (a per-target INTERFACE library, not
# something CMAKE_EXE_LINKER_FLAGS_INIT can supply globally) - so the
# generic compiler-identification try_compile CMake runs as part of
# project() fails outright otherwise ("undefined symbol: __data_start"
# from picolibc's crt0.o, which pico_standard_link's own linker script
# would normally define). Confirmed empirically: this project's own
# `cmake_minimum_required(VERSION 4.4)` (needed for `import std;`) makes
# CMake actually perform that full-link check rather than trusting a
# lesser compile-only probe the way an older `cmake_minimum_required`
# would have. A static library try-compile sidesteps the whole question.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Defaults to docker/Dockerfile's own baked-in locations; overridable for
# running this preset outside that image. Both must be propagated to
# nested try_compiles (CMake starts a fresh, mostly-empty sub-configure
# for compiler-ID/ABI detection - see toolchain-mps2an385.cmake's own
# comment on this exact CMake behavior) - PICO_TOOLCHAIN_PATH already gets
# an ENV-variable-based workaround from pico-sdk's own
# cmake/preload/toolchains/util/find_compiler.cmake, but
# PICO_COMPILER_SYSROOT has no such bridge, so without this `list(APPEND
# CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ...)` the nested try_compile falls
# back to pico-sdk's own auto-search (which only looks for
# `armv6m_soft_nofp`/`armv6m_soft_nofp_size`/`armv6m-unknown-none-eabi` -
# not this project's `_exn_rtti` variant) and fails outright.
if(NOT DEFINED RP2040_TOOLCHAIN_PATH)
  set(RP2040_TOOLCHAIN_PATH "/opt/rp2040-toolchain")
endif()
if(NOT DEFINED RP2040_SYSROOT)
  set(RP2040_SYSROOT "${RP2040_TOOLCHAIN_PATH}/lib/clang-runtimes/arm-none-eabi/armv6m_soft_nofp_exn_rtti")
endif()
set(PICO_TOOLCHAIN_PATH "${RP2040_TOOLCHAIN_PATH}")
set(PICO_COMPILER_SYSROOT "${RP2040_SYSROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PICO_COMPILER_SYSROOT PICO_TOOLCHAIN_PATH)

include(${PICO_SDK_PATH}/cmake/preload/toolchains/pico_arm_cortex_m0plus_clang.cmake)

# -nostdinc++ + explicit -isystem: pico-sdk's own toolchain file relies on
# `--sysroot` alone to steer libc++ header lookup, which works for a
# from-scratch/minimal Clang install but not this image's Debian-packaged
# libc++-${LLVM_VERSION}-dev - that packaging bakes an absolute
# `/usr/lib/llvm-${LLVM_VERSION}/include/c++/v1` search path into Clang
# regardless of --sysroot, so without this, `<stdbool.h>` (pulled in via
# picolibc's own headers) resolves to the *system* libc++'s __config
# instead of this sysroot's own, which then fails outright (no thread API
# - the system libc++ build assumes a hosted target). Same root cause
# toolchain-mps2an385.cmake's own -nostdinc++ already documents for
# armv7m; empirically reproduced and fixed the same way here.
string(APPEND CMAKE_CXX_FLAGS_INIT
  " -nostdinc++ -isystem ${PICO_COMPILER_SYSROOT}/include/c++/v1 -isystem ${PICO_COMPILER_SYSROOT}/include")

# CMAKE_CXX_STDLIB_MODULES_JSON: without this, CMake falls back to
# discovering the *system* Clang's own libc++.modules.json (via
# `clang++ -print-file-name=libc++.modules.json`, same mechanism
# toolchain-hosted-linux.cmake's own comment documents) and compiles
# *that* std.cppm - which is Clang ${LLVM_VERSION}'s own, newer than what
# this sysroot's headers/libraries actually are (its __config reports
# _LIBCPP_VERSION 190105 - ARM's release bundles LLVM 19.1.5's libc++).
# Pairing the *system* std.cppm (which unconditionally includes
# <flat_map>, added to libc++ upstream after 19.1.5) against this
# sysroot's own older headers doesn't just fail to find <flat_map> - it's
# a real, unfixable-by-patching-around-it incompatibility once anything
# tries to bridge the two (confirmed empirically: adding a fallback
# -isystem for just the missing headers instead pulls in a chain of other
# newer transitive headers whose private ABI macros don't match what this
# sysroot's own __config already defined). Same fix
# toolchain-mps2an385.cmake's own identical line already uses: point this
# at the sysroot's *own* libc++.modules.json/std.cppm, generated against
# the exact same 19.1.5 headers/libraries this sysroot has, so the
# version question never arises - Clang ${LLVM_VERSION} is simply
# cross-compiling target code for an older target libc++, same as any
# other cross-compile in this repo.
set(CMAKE_CXX_STDLIB_MODULES_JSON "${PICO_COMPILER_SYSROOT}/lib/libc++.modules.json")

# -Wno-reserved-module-identifier: same reasoning as
# toolchain-hosted-linux.cmake's own identical flag - needed for `est`'s
# own `import std;` (this project links `est` itself, not just pico-sdk).
string(APPEND CMAKE_CXX_FLAGS_INIT " -Wno-reserved-module-identifier")

# -fuse-ld=lld: pico-sdk's own toolchain file leaves linker selection to
# Clang's default, which on this image resolves to GNU ld (bfd) - that
# linker doesn't understand the `armelf` emulation Clang requests for this
# target ("unrecognised emulation mode: armelf"). This image's own lld
# (not the sysroot's, which has none) is what every other bare-metal
# target in this repo already links with.
foreach(TYPE IN ITEMS EXE SHARED MODULE)
  string(APPEND CMAKE_${TYPE}_LINKER_FLAGS_INIT " -fuse-ld=lld")
endforeach()

# --rtlib=compiler-rt/--unwindlib=libunwind: pico-sdk's own toolchain file
# doesn't select a runtime library, so Clang falls back to its GCC-style
# default (`-lgcc`/`-lgcc_s`) - this sysroot ships neither (it's a
# clang_rt.builtins/libunwind sysroot, same as every other target in this
# repo), so the final link fails with "unable to find library -lgcc".
# Steering Clang to the runtime the sysroot actually has fixes it; the
# per-language compile flag is accepted-but-unused (only the link step
# needs it) and kept only for consistency with the linker flags.
foreach(LANG IN ITEMS C CXX ASM)
  string(APPEND CMAKE_${LANG}_FLAGS_INIT " --rtlib=compiler-rt")
endforeach()
foreach(TYPE IN ITEMS EXE SHARED MODULE)
  string(APPEND CMAKE_${TYPE}_LINKER_FLAGS_INIT " --rtlib=compiler-rt --unwindlib=libunwind")
endforeach()

# Explicit -lc++/-lc++abi/-lunwind: pico-sdk's own toolchain file passes
# -nostdlib at link time (its own set_flags.cmake comment: avoiding
# picolibc's default crt0/exit machinery, which this board's own boot2/
# vector table replaces) - that also disables Clang's usual *automatic*
# linking of whatever runtime a target's own flags imply, the same
# "nothing implicit, everything explicit" shape
# toolchain-mps2an385.cmake's own -nostdlib block already documents.
# libc++/libc++abi/libunwind's own exception-handling runtime symbols
# (`operator new`, `__cxa_throw`, `typeinfo`/vtable for
# std::runtime_error, ...) never had anywhere to come from until this -
# confirmed empirically once PICO_CXX_ENABLE_EXCEPTIONS=1
# (examples/rp2040/CMakeLists.txt) started actually emitting code that
# needs them. `-lc` explicitly listed too, *after* -lc++abi/-lunwind,
# matching toolchain-mps2an385.cmake's own exact ordering - Clang's
# baremetal driver logic auto-appends an implicit "-lc" of its own at
# the very end of the real invoked command regardless, but that's *too
# late* for libc++abi.a's own need for a handful of plain libc symbols
# (__cxa_thread_atexit, confirmed empirically) that a single left-to-right
# linker pass won't go back for - an explicit, correctly-ordered "-lc"
# here resolves them at the point they're actually needed.
foreach(TYPE IN ITEMS EXE SHARED MODULE)
  string(APPEND CMAKE_${TYPE}_LINKER_FLAGS_INIT
    " -L${PICO_COMPILER_SYSROOT}/lib -lc++ -lc++abi -lunwind -lc -lclang_rt.builtins -lm")
endforeach()

# -DEST_NO_THREADS: same reasoning as toolchain-mps2an385.cmake's own
# identical macro - this picolibc-based sysroot has no OS thread creation
# primitive either.
string(APPEND CMAKE_CXX_FLAGS_INIT " -DEST_NO_THREADS")

# Real exceptions/RTTI: unlike toolchain-mps2an385.cmake, this does *not*
# add -fexceptions/-frtti here - pico-sdk's own C++ targets link an
# INTERFACE library (pico_cxx_options) that appends its *own*
# -fno-exceptions/-fno-rtti (unless PICO_CXX_ENABLE_EXCEPTIONS/
# PICO_CXX_ENABLE_RTTI are set before pico_sdk_init(), examples/rp2040/
# CMakeLists.txt's own job) *after* whatever this toolchain file sets in
# CMAKE_CXX_FLAGS_INIT on the actual command line - confirmed empirically
# (an earlier attempt to add -fexceptions -frtti here lost outright:
# `-fexceptions -frtti ... -fno-exceptions -fno-rtti`, last flag wins for
# Clang). est genuinely needs real exceptions/RTTI (future<T>'s own
# exception propagation, est::check(), platform_rp2040.cppm's own
# unguarded try/catch in pump_task_loop()) - the *only* place that can
# actually win is pico-sdk's own sanctioned opt-in, not this file.

# Same per-CMake-release activation UUID as the other toolchain files -
# see toolchain-hosted-linux.cmake's own comment. Needed for `est`'s own
# `import std;` (pico-sdk itself is plain C/C++, not modules - this gate
# only matters once `est` enters the link).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

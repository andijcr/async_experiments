# CMake toolchain file for examples/multicolor_larson_scanner/web's
# wasm32-wasip1-threads build - a sibling to toolchain-hosted-linux.cmake,
# per that file's own top comment inviting exactly this for a target it
# was never meant to describe.
#
# Still this image's pinned Clang (docker/Dockerfile's CMAKE_CXX_COMPILER
# below is unversioned "clang++", the same update-alternatives-resolved
# binary the hosted toolchain uses) - only the sysroot/resource dir change,
# cross-compiling the same way any --target=/--sysroot= cross-compile
# does. Never wasi-sdk's own bundled Clang.
#
# Applied via the "wasm32" preset in
# examples/multicolor_larson_scanner/web/CMakePresets.json - that
# subdirectory is a fully separate CMake project (its own project(), its
# own presets file), not add_subdirectory()'d into the root build: CMake
# has no supported way to compile different targets through two different
# compilers/target-triples in one configure/generate pass, and the root
# CMakeLists.txt's own CMAKE_CXX_MODULE_STD/CMAKE_CXX_SCAN_FOR_MODULES
# project-wide settings assume the one hosted-Linux toolchain throughout.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_SYSROOT "/opt/wasi-sysroot" CACHE PATH "wasi-sdk sysroot (docker/Dockerfile)")

set(CMAKE_C_COMPILER_TARGET wasm32-wasip1-threads)
set(CMAKE_CXX_COMPILER_TARGET wasm32-wasip1-threads)

# The `noeh` (not `eh`) sysroot variant, with -fno-exceptions: this
# project's pinned Clang 22.1.8 has two confirmed, currently-open upstream
# LLVM WebAssembly-backend bugs that crash the compiler on real code when
# -mexception-handling is genuinely active - llvm/llvm-project#208409 (any
# real C++20 coroutine body, at -O0) and llvm/llvm-project#148550 (an
# EarlyCSE crash on ordinary std::istringstream usage, at -O1/-O2). Both
# reproduce on LLVM main/23.0.0git too (docs/PLAN.md's "Issue #99" entry),
# so no Clang version bump routes around them. -fno-exceptions sidesteps
# both: neither bug triggers without the real wasm exception-handling
# target-feature enabled. est's own future.cppm/timer_periodic.cppm/
# platform.cppm gate their try/catch behind the standard __cpp_exceptions
# feature-test macro specifically so they still compile here - see
# future.cppm's own concrete_continuation<Fn, U>::run() comment for why
# this loses nothing: -fno-exceptions makes `throw` illegal everywhere in
# the TU, so those catch blocks were already unreachable dead code on this
# target, not a behavior being given up.
set(WASM32_TARGET_INCLUDE "${CMAKE_SYSROOT}/include/wasm32-wasip1-threads/noeh")

# CMake >= 4.2's CMAKE_CXX_STDLIB_MODULES_JSON/CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
# are what make cross-compiling `import std;` at all possible here - see
# toolchain-hosted-linux.cmake's own CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
# comment for the general mechanism, and docs/PLAN.md's "Issue #99" entry
# for the full story of why this needs CMake >= 4.2 specifically: without
# CMAKE_CXX_STDLIB_MODULES_JSON, CMake locates the std module's sources via
# `clang++ -print-resource-dir`, which reports the compiler's own built-in
# default unconditionally - ignoring any -resource-dir=/--sysroot=/
# --target= passed alongside it - so it always resolved and compiled the
# *host's* own libc++ sources against wasm32 flags instead of this
# sysroot's. CMAKE_CXX_STDLIB_MODULES_JSON bypasses that broken
# auto-detection with a direct path; CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
# fixes a matching gap in CMake's own internal `#include <version>`
# stdlib-detection probe.
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
  "${WASM32_TARGET_INCLUDE}/c++/v1"
  "${WASM32_TARGET_INCLUDE}")
set(CMAKE_CXX_STDLIB_MODULES_JSON
  "${CMAKE_SYSROOT}/lib/wasm32-wasip1-threads/noeh/libc++.modules.json")

# -resource-dir=/opt/wasi-resource-wasm32: docker/Dockerfile synthesizes
# this directory specifically so Clang's own implicit compiler-rt
# builtins auto-link (libclang_rt.builtins.a) finds the wasi-sdk-provided
# one instead of failing outright (this image's own Clang has no wasm32
# builtins of its own) - see that file's own comment for why a plain
# `-L`/`-l` pair isn't enough on its own (Clang resolves the builtins
# archive via -resource-dir, not ordinary library search).
#
# -D__wasm_exception_handling__=1, NOT -mexception-handling: wasi-libc's
# own <csetjmp>/<setjmp.h> hard-#errors without this exact macro defined,
# entirely independently of -fexceptions/-fno-exceptions (a `#include
# <csetjmp>` reaches transitively from `import std;`'s own std.cppm even
# though nothing here calls setjmp/longjmp). Defining only the macro, not
# passing the real -mexception-handling flag, satisfies that guard without
# enabling the wasm target-feature the two crashes above need to trigger.
#
# -D_WASI_EMULATED_SIGNAL / -lwasi-emulated-signal: wasi-libc's own
# <csignal>/<signal.h> hard-#error without these - wasm has no real
# signal delivery, this is wasi-libc's own documented minimal-emulation
# opt-in (est/larson_scanner don't use signals; this only exists to let
# libc++ headers that #include <csignal> transitively compile/link).
#
# -pthread -matomics -mbulk-memory: WebAssembly threads support - shared
# linear memory and atomics, the mechanism the Worker/main-thread design
# (docs/PLAN.md's Issue #99 entry) depends on for est::spsc_ring<T>/
# est::external_event<T> to work across the two instances for real.
#
# -mexec-model=reactor -Wl,--no-entry: this .wasm has no C/C++ main() at
# all - it's a library of exported entry points (boot()/push_command(),
# wasm_exports.cpp) JS calls directly, the same shape a shared library
# would be, not a standalone program with a single blocking main().
# -mexec-model=reactor is link-only (rejected as a compile-time flag).
#
# -Wl,--shared-memory: marks the module's own linear memory as shared
# rather than each instantiation getting an independent copy - the
# prerequisite this whole design rests on (proved viable by the M0.5
# spike, docs/PLAN.md's Issue #99 entry).
#
# -Wl,--import-memory: without this, wasm-ld still *defines* (and
# exports) the memory itself - each WebAssembly.instantiate() call would
# then get its own independent memory regardless of --shared-memory,
# defeating the whole point. This is what makes the memory an *import*
# instead, so the embedding JS creates exactly one WebAssembly.Memory
# and passes the same object into both the Worker's and the main
# thread's own instantiate() calls (confirmed via
# WebAssembly.Module.exports()/imports() against the actual built
# binary - 'memory' moved from the export list to the import list once
# this flag was added).
#
# -Wl,--initial-memory=/-Wl,--max-memory=: shared memory specifically
# requires an explicit, fixed max (a SharedArrayBuffer can't be
# unbounded) - 16 initial pages (1MiB)/256 max pages (16MiB) is
# generous for this app's own small, fixed-size state (a `width`-wide
# led_buffer, a handful of coroutine frames, spsc_ring<command>'s fixed
# capacity). Must match what the embedding JS's own `new
# WebAssembly.Memory({initial, maximum, shared: true})` declares.
#
# -Wno-reserved-module-identifier: same reasoning as
# toolchain-hosted-linux.cmake's own identical flag - CMake >= 4.2's
# synthetic per-consuming-target std.cppm/std.compat.cppm compile inherits
# a consuming target's own -Werror, and libc++'s `export module std;`
# unconditionally triggers this diagnostic regardless of warning level.
set(CMAKE_CXX_FLAGS_INIT
  "-stdlib=libc++ -pthread -matomics -mbulk-memory -fno-exceptions -fno-rtti -D__wasm_exception_handling__=1 -D_WASI_EMULATED_SIGNAL -Wno-reserved-module-identifier -resource-dir=/opt/wasi-resource-wasm32 -isystem ${WASM32_TARGET_INCLUDE}/c++/v1 -isystem ${WASM32_TARGET_INCLUDE}")
set(CMAKE_EXE_LINKER_FLAGS_INIT
  "-stdlib=libc++ -pthread -matomics -mbulk-memory -fno-exceptions -fno-rtti -mexec-model=reactor -Wl,--no-entry -Wl,--shared-memory -Wl,--import-memory -Wl,--initial-memory=1048576 -Wl,--max-memory=16777216 -L${CMAKE_SYSROOT}/lib/wasm32-wasip1-threads/noeh -resource-dir=/opt/wasi-resource-wasm32 -lwasi-emulated-signal")

# Same per-CMake-release activation UUID as toolchain-hosted-linux.cmake -
# see that file's own comment. Kept in sync manually (both toolchain files
# are pinned to this repo's one CMAKE_VERSION); a mismatch here would show
# up immediately as a hard configure-time error ("incorrect value"), not a
# silent gap.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

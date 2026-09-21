# CMake toolchain file for examples/multicolor_larson_scanner/mps2an385's
# bare-metal QEMU build - a sibling to toolchain-wasm32.cmake, and built
# the same way: this image's own pinned Clang ${LLVM_VERSION}
# (docker/Dockerfile) does the actual compiling, cross-compiling via
# --target=/-isystem=/-L= against a prebuilt sysroot, not a wholly
# separate compiler distribution.
#
# The sysroot is ARM's own prebuilt "LLVM Embedded Toolchain for Arm"
# release (docker/Dockerfile's own /opt/arm-none-eabi-sysroot block has
# the full provenance) - picolibc-based libc/libc++/libc++abi/libunwind
# for armv7m-none-eabi, with real exceptions/RTTI enabled. Deliberately
# NOT that release's own bundled Clang 19.1.5 binary, which this image
# never installs at all: issue #123's PR-design comment has the full
# story, but in short, that older Clang can't compile `import std;`
# alongside a textual #include of the same header in one TU (reproduces
# with nothing but `import std; #include <string>;`) - a combination
# every one of est's own test files needs (Catch2's TEST_CASE macros
# need a textual #include; est:: needs `import est;`, which itself does
# `import std;`). This image's newer Clang handles the same combination
# correctly, verified against this exact sysroot.
#
# Applied via the "mps2an385" preset in
# examples/multicolor_larson_scanner/mps2an385/CMakePresets.json - that
# subdirectory is a fully separate CMake project (its own project(), its
# own presets file), not add_subdirectory()'d into the root build, for
# the same reason toolchain-wasm32.cmake's own top comment gives.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# A full EXECUTABLE try-compile needs this project's own -T link.ld
# (board memory layout/vector-table placement) to succeed meaningfully;
# CMake's internal compiler-identification/feature-check try-compiles
# never see that flag (only this file's own *_INIT variables). A static
# library try-compile sidesteps the whole question - standard practice
# for a bare-metal toolchain file, and this project's real add_executable
# calls (which do carry -T) still surface any genuine link failure
# immediately.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Defaults to docker/Dockerfile's own baked-in location; overridable
# (e.g. -DMPS2AN385_SYSROOT=/path/to/armv7m_soft_nofp_exn_rtti) for
# running this preset outside that image.
if(NOT DEFINED MPS2AN385_SYSROOT)
  set(MPS2AN385_SYSROOT "/opt/arm-none-eabi-sysroot")
endif()
# A nested try_compile (compiler-ID/ABI detection included) starts a
# fresh CMake configure that does NOT automatically inherit a custom
# cache variable like MPS2AN385_SYSROOT from the outer build's own
# CMakeCache.txt - CMAKE_TOOLCHAIN_FILE itself is reused (so this file
# runs again for that nested configure too), but without this, that
# second run would fall back to the default above even when the caller
# explicitly overrode it.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MPS2AN385_SYSROOT)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_C_COMPILER_TARGET armv7m-none-eabi)
set(CMAKE_CXX_COMPILER_TARGET armv7m-none-eabi)

# -fexceptions -frtti (not -fno-*): the whole point of issue #123's
# Findings 7-9 - real exceptions/RTTI work on this target given the
# right EHABI sections (picolibcpp.ld, referenced from this project's own
# src/link.ld) and `--target2=rel` (Finding 7's own comment has the full
# story: ld.lld's default R_ARM_TARGET2 encoding is GOT-indirected, but
# the EHABI unwinder always decodes a catch clause's type_info pointer as
# a *direct* PREL31 offset - the mismatch silently breaks every typed
# catch, not just some).
#
# -Wno-reserved-module-identifier: same reasoning as
# toolchain-hosted-linux.cmake's own identical flag.
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
  "${MPS2AN385_SYSROOT}/include/c++/v1"
  "${MPS2AN385_SYSROOT}/include")
set(CMAKE_CXX_STDLIB_MODULES_JSON "${MPS2AN385_SYSROOT}/lib/libc++.modules.json")

set(CMAKE_C_FLAGS_INIT
  "-march=armv7m -mfpu=none -mfloat-abi=soft")
# -DEST_NO_THREADS: this runtime is built with _LIBCPP_HAS_NO_THREADS
# (freestanding, no OS thread creation primitive at all - unlike
# toolchain-wasm32.cmake's target, which genuinely has real OS-level
# threads via WebAssembly threads/Worker) - <thread>/std::this_thread
# don't exist here. A project-owned macro rather than testing libc++'s
# own internal _LIBCPP_HAS_NO_THREADS directly from est's source, so the
# gate stays meaningful even if a future backend's underlying stdlib
# reason for lacking threads differs. larson_scanner_app.cppm's own
# push_command() is the one place in this codebase that currently needs
# it (its std::this_thread::yield() backoff hint has nothing to yield to
# on a target with no threads at all).
set(CMAKE_CXX_FLAGS_INIT
  "-march=armv7m -mfpu=none -mfloat-abi=soft -stdlib=libc++ -fexceptions -frtti -nostdinc++ -isystem ${MPS2AN385_SYSROOT}/include/c++/v1 -isystem ${MPS2AN385_SYSROOT}/include -Wno-reserved-module-identifier -DEST_NO_THREADS")

# -nostdlib: this board provides its own Reset_Handler/vector table (not
# picolibc's crt0.o) - see startup.c's own top comment - so none of
# picolibc's/libc++'s usual implicit startup-object linking applies;
# every runtime library below is named explicitly instead. -fuse-ld=lld:
# this image's own lld (not the sysroot's, which has none), same as
# toolchain-wasm32.cmake.
# -Wl,--target2=rel: the Finding 7 fix, see above.
set(CMAKE_EXE_LINKER_FLAGS_INIT
  "-march=armv7m -mfpu=none -mfloat-abi=soft -nostdlib -fuse-ld=lld -Wl,--target2=rel -L${MPS2AN385_SYSROOT}/lib -lc++ -lc++abi -lunwind -lc -lclang_rt.builtins -lm")

# Same per-CMake-release activation UUID as the other toolchain files -
# see toolchain-hosted-linux.cmake's own comment.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

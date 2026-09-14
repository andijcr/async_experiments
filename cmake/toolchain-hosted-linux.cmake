# CMake toolchain file for the pinned devenv Docker image
# (docker/Dockerfile): Clang + libc++ on hosted Linux.
#
# Named "hosted-linux" so a future bare-metal backend gets its own sibling
# toolchain file rather than this one growing #if-ery for a target it was
# never meant to describe.
#
# Applied via the "default"/"ci" CMake presets (CMakePresets.json) rather
# than CC/CXX environment variables baked into the image, so the compiler/
# stdlib pin is declared once, in the repo.

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

# import std;/<print> (C++23) need libc++ specifically - Clang otherwise
# defaults to whatever libstdc++ is on the system, and the devenv image
# doesn't even have one (see docker/Dockerfile). CMAKE_CXX_FLAGS_INIT/
# CMAKE_EXE_LINKER_FLAGS_INIT (rather than CMAKE_CXX_FLAGS directly) is
# the toolchain-file-correct way to seed this: it's merged with, not
# overwritten by, whatever a caller passes on the command line.
#
# -static-libstdc++ (linker flags only - clang understands this
# GCC-originated spelling for libc++ too, statically linking libc++ *and*
# libc++abi instead of the devenv's own libc++-22-dev-packaged .so):
# Debian's packaged libc++.so.1 doesn't export a handful of hidden-
# visibility helper symbols (__atomic_monitor_global() and friends,
# behind an internal `[abi:nqe...]` tag) that std::atomic<T>::wait()/
# notify_*() - and therefore std::jthread's stop_token machinery - need
# at link time, even though the identical libc++.a *does* contain them
# (an archive's member object files keep every global symbol regardless
# of the visibility attributes that gate what a .so exports). Confirmed
# by diffing `nm -D` against a plain `nm` on the two forms of the same
# package/version - not a version-skew issue between two different
# libc++ builds, just what this Debian package chooses to export
# dynamically. Statically linking sidesteps the gap entirely rather than
# working around it per-target.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -static-libstdc++")

# Gates CMake's experimental `import std;` support. This value is
# specific to the CMake release range it was validated against
# (3.30.0-3.31.7); docker/Dockerfile's pinned CMAKE_VERSION (3.31.0)
# falls inside that range. Must be set before project() - a toolchain
# file's content runs at exactly that point, which is why this lives
# here and not in the top-level CMakeLists.txt (CMAKE_CXX_MODULE_STD,
# the project-level opt-in that actually requests the std module once
# this gate allows it, is set there instead).
#
# Depends on docker/Dockerfile's `/usr/lib/share/libc++/v1` symlink:
# Debian's libc++-${LLVM_VERSION}-dev package ships a
# `libc++.modules.json` whose `source-path` for the std module resolves
# relative to a flat install prefix Debian's packaging doesn't actually
# have, so that symlink is what makes the real, versioned module sources
# resolvable. See that file's comment for the layout details.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")

# CMake toolchain file for the pinned devenv Docker image
# (docker/Dockerfile): Clang + libc++ on hosted Linux.
#
# This is the "hosted-Linux" backend referred to throughout docs/PLAN.md
# (est::platform's only current implementation) - named accordingly so a
# future bare-metal backend gets its own sibling toolchain file here
# (docs/PLAN.md, "Stretch / explicitly deferred") rather than this one
# growing #if-ery for a target it was never meant to describe.
#
# Used via the "default"/"ci" CMake presets (CMakePresets.json), not via
# CC/CXX environment variables baked into the image, so the compiler/
# stdlib pin is declared once, in the repo, instead of being implicit in
# whatever the container's shell environment happens to set up.

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

# import std;/<print> (C++23) need libc++ specifically - Clang otherwise
# defaults to whatever libstdc++ is on the system, and the devenv image
# doesn't even have one (see docker/Dockerfile). CMAKE_CXX_FLAGS_INIT/
# CMAKE_EXE_LINKER_FLAGS_INIT (rather than CMAKE_CXX_FLAGS directly) is
# the toolchain-file-correct way to seed this: it's merged with, not
# overwritten by, whatever a caller passes on the command line.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++")

# TODO: pin CMAKE_EXPERIMENTAL_CXX_IMPORT_STD to the gate value validated
# for docker/Dockerfile's pinned CMAKE_VERSION once that pin itself exists
# - see docs/PLAN.md, "Known open items" (apt.llvm.org/cmake.org were
# unreachable from the session that wrote this scaffolding, so neither
# pin could be looked up or validated).
# set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "<gate-value-for-pinned-cmake-version>")

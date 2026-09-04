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

# `import std;` was tried project-wide (docs/PLAN.md, M2 review round 5)
# and reverted - not still unverified, but confirmed broken with this
# image as currently built. CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
# "0e5b6991-d74f-4b3d-a41c-cf096e0b2508" (the correct gate value for our
# pinned CMAKE_VERSION 3.31.0) was accepted fine and Clang 22.1.8 was
# detected correctly, but configure then failed:
#   CMake Error: Cannot find source file: /lib/share/libc++/v1/std.cppm
# i.e. the devenv image's installed libc++ package doesn't actually ship
# the std module's own source file at the path CMake expects - a
# packaging gap in docker/Dockerfile's `libc++-${LLVM_VERSION}-dev`
# install (that file's own comment already flagged the package names as
# unconfirmed - this is that risk landing). Fixing it means finding
# which apt.llvm.org package (if any, for this LLVM_VERSION/Debian
# combination) actually provides std.cppm and adjusting the Dockerfile -
# apt.llvm.org is blocked by this sandbox's network egress policy, so
# that investigation could not be done as part of this finding.
# set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")

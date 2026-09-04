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

# Gates CMake's experimental `import std;` support. This value is
# specific to the CMake release range it was validated against
# (3.30.0-3.31.7); docker/Dockerfile's pinned CMAKE_VERSION (3.31.0)
# falls inside that range. Must be set before project() - a toolchain
# file's content runs at exactly that point, which is why this lives
# here and not in the top-level CMakeLists.txt (CMAKE_CXX_MODULE_STD,
# the project-level opt-in that actually requests the std module once
# this gate allows it, is set there instead).
#
# `import std;` was tried project-wide once before (docs/PLAN.md, M2
# review round 5) and reverted: this gate and Clang 22.1.8 detection both
# worked, but configure then failed with "Cannot find source file:
# /lib/share/libc++/v1/std.cppm" - Debian's libc++-${LLVM_VERSION}-dev
# package installs `libc++.modules.json` at the standard multiarch path
# (so plain `-stdlib=libc++` finds it without needing the versioned
# resource dir), but that JSON's `source-path` is relative to its own
# directory and assumes a flat install prefix that Debian's packaging
# doesn't actually have - `share/libc++/v1/` only exists under the
# versioned `/usr/lib/llvm-${LLVM_VERSION}/`, not mirrored next to the
# multiarch lib dir. docker/Dockerfile now symlinks
# `/usr/lib/share/libc++/v1` to the real, versioned directory to paper
# over this packaging gap, confirmed working end-to-end (configure +
# build + test) in a from-scratch `docker build` - see docs/PLAN.md,
# "import std; re-adopted, project-wide".
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")

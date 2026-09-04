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
# (3.30.0-3.31.7, confirmed via CMake's own release notes/discourse -
# docs/PLAN.md's "Known open items" TODO this replaces couldn't verify
# this directly since apt.llvm.org/cmake.org were unreachable from the
# session that wrote the original scaffolding); docker/Dockerfile's
# pinned CMAKE_VERSION (3.31.0) falls inside that range. Must be set
# before project() - a toolchain file's content runs at exactly that
# point, which is why this lives here and not in the top-level
# CMakeLists.txt (CMAKE_CXX_MODULE_STD, the project-level opt-in that
# actually requests the std module once this gate allows it, is set
# there instead).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")

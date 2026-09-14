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
#
# -Wno-reserved-module-identifier: CMake >= 4.2's CMAKE_CXX_MODULE_STD
# machinery compiles libc++'s own `std.cppm` (`export module std;`) as
# part of any target that opts into `import std;` - a synthetic,
# per-consuming-target build product this project doesn't control the
# source of. That file's own `export module std;` unconditionally
# triggers Clang's reserved-module-identifier diagnostic (not gated
# behind -Wall/-Wextra/-Wpedantic, so est_set_warnings()'s own explicit
# flag list can't be the thing disabling it) - and since CMake 4.x's
# synthetic std-module target inherits a consuming target's own PRIVATE
# compile options (confirmed via CMakeConfigureLog.yaml: the failing
# command line carries both this file's own flags and est_set_warnings()'s
# -Werror), that library's own unavoidable warning becomes a hard build
# failure for every target using `import std;` unless silenced here,
# before any target's own -Werror can see it.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++ -Wno-reserved-module-identifier")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -static-libstdc++")

# Gates CMake's experimental `import std;` support. This UUID is the
# per-release activation value CMake documents for its own version
# (Help/dev/experimental.rst in CMake's own source tree) - it changes
# whenever the experimental feature's shape changes, so it's tied to
# docker/Dockerfile's pinned CMAKE_VERSION, not portable across CMake
# versions on its own. Current value is CMake 4.4.0's. Must be set
# before project() - a toolchain file's content runs at exactly that
# point, which is why this lives here and not in the top-level
# CMakeLists.txt (CMAKE_CXX_MODULE_STD, the project-level opt-in that
# actually requests the std module once this gate allows it, is set
# there instead).
#
# Depends on docker/Dockerfile's `/usr/lib/share/libc++/v1` symlink:
# Debian's libc++-${LLVM_VERSION}-dev package ships a
# `libc++.modules.json` whose `source-path` for the std module resolves
# relative to a flat install prefix Debian's packaging doesn't actually
# have, so that symlink is what makes the real, versioned module sources
# resolvable. See that file's comment for the layout details.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

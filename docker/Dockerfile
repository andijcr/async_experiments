# Dev/CI image for `est`: one image used both for local development and as
# the container GitHub Actions builds/lints/tests inside. This Dockerfile
# only installs the toolchain (Clang, libc++, CMake, Ninja) - selecting
# clang/clang++ and -stdlib=libc++ is cmake/toolchain-hosted-linux.cmake's
# job, applied via the "default"/"ci" CMake presets, not this image's
# environment.
#
# LLVM_VERSION=22 resolves to Clang 22.1.8; libc++-22-dev/libc++abi-22-dev/
# libclang-rt-22-dev all install cleanly for this version on
# debian:bookworm-slim.

FROM debian:bookworm-slim

ARG LLVM_VERSION=22
ARG CMAKE_VERSION=4.4.0
ARG USER_UID=1000
ARG USER_GID=1000

ENV DEBIAN_FRONTEND=noninteractive

# apt.llvm.org resolves to an AAAA (IPv6) record, and GitHub-hosted Actions
# runners have no IPv6 route out of the Docker build - wget/apt then hang
# on "Network is unreachable" instead of falling back to IPv4. Force IPv4
# for both apt itself and the wget below.
RUN echo 'Acquire::ForceIPv4 "true";' > /etc/apt/apt.conf.d/99force-ipv4

# --- Base tooling needed to fetch the pinned Clang/CMake ---
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
        lsb-release \
        software-properties-common \
        wget \
        git \
        ninja-build \
        python3 \
        python3-pip \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# --- Pinned Clang/LLVM snapshot from apt.llvm.org ---
# llvm.sh is apt.llvm.org's own installer script: it adds the matching apt
# repo for the running Debian/Ubuntu release and installs the requested
# major version. `all` also pulls in clang-format, clang-tidy, lld, lldb.
RUN wget --inet4-only -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh "${LLVM_VERSION}" all \
    && rm -f /tmp/llvm.sh \
    && rm -rf /var/lib/apt/lists/*

# libc++ and compiler-rt, explicitly: this image has no gcc/libstdc++-dev
# at all, and even where a libstdc++ happens to be present Clang won't use
# it for `import std;`/<print> support without pairing it with libc++ -
# `all` above does not reliably pull in either of these. compiler-rt's
# profile runtime (libclang_rt.profile) is what -fprofile-instr-generate
# links against for the coverage gate.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        "libc++-${LLVM_VERSION}-dev" \
        "libc++abi-${LLVM_VERSION}-dev" \
        "libclang-rt-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

# Work around a Debian packaging gap: `libc++-<N>-dev` installs
# `libc++.modules.json` (the manifest CMake's `import std;` support
# reads) at the standard multiarch path, found via `clang++
# -print-file-name=libc++.modules.json` without needing the versioned
# resource dir. But that JSON's `source-path` for the std module is
# relative to its own directory and assumes a flat install prefix -
# `<prefix>/lib/<triple>/libc++.modules.json` next to
# `<prefix>/share/libc++/v1/std.cppm` - which Debian's packaging doesn't
# have: the std module's real sources only exist under the versioned
# `/usr/lib/llvm-${LLVM_VERSION}/share/libc++/v1/`. A directory symlink
# at the path the JSON's relative resolution actually looks for fixes it
# without touching the real, versioned installation.
RUN mkdir -p /usr/lib/share/libc++ \
    && ln -s "/usr/lib/llvm-${LLVM_VERSION}/share/libc++/v1" /usr/lib/share/libc++/v1

# Unversioned names (`clang`, `clang++`, `clang-format`, `clang-tidy`, ...)
# resolve to the pinned version, so the rest of this Dockerfile and the CI
# workflow don't need to know LLVM_VERSION.
RUN for tool in clang clang++ clang-format clang-tidy clang-scan-deps lld lldb llvm-cov llvm-profdata; do \
        update-alternatives --install "/usr/bin/${tool}" "${tool}" \
            "/usr/bin/${tool}-${LLVM_VERSION}" 100; \
    done

# diff-cover: intersects the llvm-cov lcov report with a PR's diff to
# gate on new-code coverage rather than the whole repo's. Debian's system
# Python is "externally managed" (PEP 668); this is a throwaway container
# image, not a shared system, so overriding that guard is fine here.
RUN pip install --no-cache-dir --break-system-packages diff-cover==10.5.1

# --- Pinned CMake ---
# The distro package is generally far behind the C++20/23-modules-support
# curve, so install a specific upstream release instead.
RUN arch="$(uname -m)" \
    && wget --inet4-only -O /tmp/cmake-install.sh \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.sh" \
    && chmod +x /tmp/cmake-install.sh \
    && /tmp/cmake-install.sh --skip-license --prefix=/usr/local \
    && rm -f /tmp/cmake-install.sh

# --- Pinned wasi-sdk sysroot, for examples/multicolor_larson_scanner/web's
# wasm32-wasip1-threads build (cmake/toolchain-wasm32.cmake) ---
# Not wasi-sdk's own bundled Clang - that build stays on this image's
# pinned Clang ${LLVM_VERSION} throughout, cross-compiling against this
# sysroot via --target=/--sysroot= the same way any other cross-compile
# does. Two release assets, not the ~190MB full SDK tarball: `wasi-sysroot`
# (libc/libm/libc++, headers + compiled libs, for every wasip1(-threads)
# variant) and `libclang_rt` (compiler-rt builtins only - `est`'s own
# `import std;` need libc++ but not wasi-sdk's own bundled Clang to supply
# it).
#
# The builtins archive alone isn't enough to link with, though: Clang
# auto-links `libclang_rt.builtins.a` from whatever `-resource-dir` is
# active, and that same resource dir also has to keep supplying Clang's
# own architecture-generic builtin headers (stddef.h, stdarg.h, float.h,
# ...) - dropping those (by pointing `-resource-dir` at a directory that
# only has the wasi-sdk builtins lib and nothing else) breaks every
# `#include_next <stddef.h>` in libc++'s own headers. So this synthesizes
# one self-contained resource dir for the wasm32 toolchain file to point
# at: a copy of this image's own Clang ${LLVM_VERSION} resource dir (for
# the headers) with the wasi-sdk builtins libs overlaid into its `lib/`
# (for the one directory Clang's auto-link actually needs to find
# `libclang_rt.builtins.a` under, per target triple).
ARG WASI_SDK_VERSION=34
ARG WASI_SDK_RELEASE=34.0
RUN mkdir -p /opt/wasi-sysroot /opt/wasi-resource-wasm32 \
    && wget --inet4-only -O /tmp/wasi-sysroot.tar.gz \
        "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION}/wasi-sysroot-${WASI_SDK_RELEASE}.tar.gz" \
    && tar xzf /tmp/wasi-sysroot.tar.gz -C /opt/wasi-sysroot --strip-components=1 \
    && rm -f /tmp/wasi-sysroot.tar.gz \
    && cp -r "$(clang++ -print-resource-dir)/." /opt/wasi-resource-wasm32/ \
    && wget --inet4-only -O /tmp/libclang_rt.tar.gz \
        "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION}/libclang_rt-${WASI_SDK_RELEASE}.tar.gz" \
    && tar xzf /tmp/libclang_rt.tar.gz -C /opt/wasi-resource-wasm32/lib --strip-components=1 \
    && rm -f /tmp/libclang_rt.tar.gz \
    && chmod -R a+rX /opt/wasi-sysroot /opt/wasi-resource-wasm32

# --- Pinned ARM bare-metal sysroot, for
# examples/multicolor_larson_scanner/mps2an385's armv7m-none-eabi build
# (cmake/toolchain-mps2an385.cmake) - same shape as the wasi-sysroot block
# above: this image's own pinned Clang ${LLVM_VERSION} does the actual
# compiling, cross-compiling via --target=/-isystem=/-L= against this
# sysroot; ARM's own bundled Clang from the release tarball below is
# never installed or used at all (issue #123's PR-design comment has the
# full story: ARM's own older, toolchain-bundled Clang 19.1.5 genuinely
# can't compile `import std;` alongside a textual #include of the same
# header in one TU - a real bug reproduced with nothing but `import std;
# #include <string>;` - while this image's newer Clang handles the exact
# same combination correctly, and Catch2's C++20 TEST_CASE macros need
# that combination in every one of est's own test files).
#
# Only one of the ~40 variants the release ships (armv7m_soft_nofp_exn_rtti -
# soft-float armv7-M, real exceptions/RTTI enabled) is kept; the release's
# own bundled bin/ (its Clang/lld, ~300MB, unused per above) and every
# other variant are discarded after extracting - the one variant actually
# needed is ~20MB (headers, prebuilt libc++.a/libc++abi.a/libunwind.a/
# libc.a/libclang_rt.builtins.a, and picolibcpp.ld - see
# examples/multicolor_larson_scanner/mps2an385/src/link.ld's own comment
# for why the linker script matters here too).
ARG ARM_TOOLCHAIN_VERSION=19.1.5
ARG ARM_TOOLCHAIN_VARIANT=armv7m_soft_nofp_exn_rtti
RUN mkdir -p /opt/arm-none-eabi-sysroot \
    && wget --inet4-only -O /tmp/arm-toolchain.tar.xz \
        "https://github.com/ARM-software/LLVM-embedded-toolchain-for-Arm/releases/download/release-${ARM_TOOLCHAIN_VERSION}/LLVM-ET-Arm-${ARM_TOOLCHAIN_VERSION}-Linux-x86_64.tar.xz" \
    && tar xJf /tmp/arm-toolchain.tar.xz \
        -C /opt/arm-none-eabi-sysroot --strip-components=5 \
        "LLVM-ET-Arm-${ARM_TOOLCHAIN_VERSION}-Linux-x86_64/lib/clang-runtimes/arm-none-eabi/${ARM_TOOLCHAIN_VARIANT}" \
    && rm -f /tmp/arm-toolchain.tar.xz \
    && chmod -R a+rX /opt/arm-none-eabi-sysroot

# --- Non-root user for local bind-mount ergonomics ---
RUN groupadd --gid "${USER_GID}" est \
    && useradd --uid "${USER_UID}" --gid "${USER_GID}" --create-home --shell /bin/bash est

USER est
WORKDIR /workspace

CMD ["/bin/bash"]

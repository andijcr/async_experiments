# Dev/CI image for `est`: one image used both for local development and as
# the container GitHub Actions builds/lints/tests inside. This Dockerfile
# only installs the toolchain (Clang, libc++, CMake, Ninja) - selecting
# clang/clang++ and -stdlib=libc++ is cmake/toolchain-hosted-linux.cmake's
# job, applied via the "default"/"ci" CMake presets, not this image's
# environment.
#
# The LLVM_VERSION/CMAKE_VERSION pins below were originally written blind
# (see docs/PLAN.md "Known open items" for that history) but have since
# been confirmed twice: once by real CI (docs/PLAN.md, "First real
# toolchain build") and again by a from-scratch `docker build` plus a full
# configure/build/tidy/format/test pass in a network-enabled sandbox
# (docs/PLAN.md, "Docker build verified end-to-end"). Clang resolves to
# 22.1.8; libc++-22-dev/libc++abi-22-dev/libclang-rt-22-dev all exist and
# install cleanly for this LLVM_VERSION/Debian combination.

FROM debian:bookworm-slim

ARG LLVM_VERSION=22
ARG CMAKE_VERSION=3.31.0
ARG USER_UID=1000
ARG USER_GID=1000

ENV DEBIAN_FRONTEND=noninteractive

# apt.llvm.org resolves to an AAAA (IPv6) record, and GitHub-hosted Actions
# runners have no IPv6 route out of the Docker build - wget/apt then hang
# on "Network is unreachable" instead of falling back to IPv4. Force IPv4
# for both apt itself and the wget below. (Found by the first real CI run
# on this Dockerfile - see docs/PLAN.md "Revised after the first real CI
# run".)
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
# links against for the coverage gate below. Package names follow
# apt.llvm.org's usual libc++-<N>-dev/libc++abi-<N>-dev/libclang-rt-<N>-dev
# convention - confirmed to exist and install cleanly for LLVM_VERSION=22
# on debian:bookworm-slim (see docs/PLAN.md, "Docker build verified
# end-to-end").
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        "libc++-${LLVM_VERSION}-dev" \
        "libc++abi-${LLVM_VERSION}-dev" \
        "libclang-rt-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

# Unversioned names (`clang`, `clang++`, `clang-format`, `clang-tidy`, ...)
# resolve to the pinned version, so the rest of this Dockerfile and the CI
# workflow don't need to know LLVM_VERSION.
RUN for tool in clang clang++ clang-format clang-tidy clang-scan-deps lld lldb llvm-cov llvm-profdata; do \
        update-alternatives --install "/usr/bin/${tool}" "${tool}" \
            "/usr/bin/${tool}-${LLVM_VERSION}" 100; \
    done

# diff-cover: intersects the llvm-cov lcov report below with a PR's diff
# to gate on new-code coverage rather than the whole repo's. Pinned to
# match what this session verified the coverage pipeline against locally.
# Debian's system Python is "externally managed" (PEP 668); this is a
# throwaway container image, not a shared system, so overriding that
# guard is the right call here, not a workaround to avoid fixing properly.
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

# --- Non-root user for local bind-mount ergonomics ---
RUN groupadd --gid "${USER_GID}" est \
    && useradd --uid "${USER_UID}" --gid "${USER_GID}" --create-home --shell /bin/bash est

USER est
WORKDIR /workspace

CMD ["/bin/bash"]

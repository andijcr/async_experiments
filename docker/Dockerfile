# Dev/CI image for `est`: one image used both for local development and as
# the container GitHub Actions builds/lints/tests inside. This Dockerfile
# only installs the toolchain (Clang, libc++, CMake, Ninja) - selecting
# clang/clang++ and -stdlib=libc++ is cmake/toolchain-hosted-linux.cmake's
# job, applied via the "default"/"ci" CMake presets, not this image's
# environment. See docs/PLAN.md ("Docker strategy" / "Known open items")
# for why the version pins below are TODOs rather than filled-in values:
# this scaffolding was written from a session whose network egress policy
# blocked apt.llvm.org and cmake.org, so the exact snapshot/version numbers
# below could not be looked up or validated by a real `docker build`.
#
# TODO: pin LLVM_VERSION to the actual current 22 (or 23 trunk) snapshot
# apt.llvm.org serves, and CMAKE_VERSION to a release whose
# CMAKE_EXPERIMENTAL_CXX_IMPORT_STD gate value has been confirmed to work
# with that Clang (record the matching gate value in the top-level
# CMakeLists.txt at the same time — see the TODO there).

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

# libc++, explicitly: this image has no gcc/libstdc++-dev at all, and even
# where a libstdc++ happens to be present Clang won't use it for
# `import std;`/<print> support without pairing it with libc++ - `all`
# above does not reliably pull in libc++'s own dev packages. Package names
# below follow apt.llvm.org's usual libc++-<N>-dev/libc++abi-<N>-dev
# convention but were not confirmed against a real `apt-cache search`
# (this session couldn't reach apt.llvm.org directly - see docs/PLAN.md,
# "Known open items"). Verify these package names exist for the pinned
# LLVM_VERSION when this image is first actually built.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        "libc++-${LLVM_VERSION}-dev" \
        "libc++abi-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

# Unversioned names (`clang`, `clang++`, `clang-format`, `clang-tidy`, ...)
# resolve to the pinned version, so the rest of this Dockerfile and the CI
# workflow don't need to know LLVM_VERSION.
RUN for tool in clang clang++ clang-format clang-tidy clang-scan-deps lld lldb; do \
        update-alternatives --install "/usr/bin/${tool}" "${tool}" \
            "/usr/bin/${tool}-${LLVM_VERSION}" 100; \
    done

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

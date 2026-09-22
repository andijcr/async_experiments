# `est` — an educational async framework for C++23/26

This file is the project's full decision log — every milestone, every
reviewed design decision, every bug found and fixed, in the order it
happened. For a reader's guide to *how the code works right now* (module
architecture, the continuation-node mechanism, allocation patterns for
`.then()` chains, the run loop) see [`docs/wiki/`](wiki/Home.md) instead —
it summarizes and cross-references this file rather than duplicating it.

## Context

This is a from-scratch educational project: a coroutine-friendly async
framework (namespace `est`) built to learn modern C++ (23/26), std modules,
and single-threaded event-loop design in depth.

Design constraints, settled up front:
- Single-core, single-threaded loop runner. The loop's own design has no
  cross-thread shared memory and no multi-shard/multi-core story (yet — see
  Stretch goals).
- Custom allocator support from the start (loop internals, shared state,
  coroutine frames), not bolted on later.
- CMake-based, Clang 22/23, std modules used as fully as the toolchain
  allows.
- CI on GitHub Actions gates PRs on `clang-format`, `clang-tidy`, and unit
  tests.
- A single Docker image is used for both local dev and CI (build + lint),
  so "it builds in CI" and "it builds on my machine" are the same claim.
- Static linking as the primary/default mode.
- Repo layout: a platform-agnostic framework + a hello-world example app.
- Build order: platform seam + timers + a minimal mutex first, then
  future/promise/continuation, then a single-threaded looper that owns and
  drives futures, then coroutine adapters on top.
- Futures/promises are *views* over a heap-allocated, ref-counted shared
  state. An abandoned future (dropped without being awaited) does not cancel
  the work — the loop itself holds a reference to the shared state and keeps
  driving it to completion in the background; only the caller's handle to
  the result is gone.
- The framework must not bake in hosted-OS assumptions: the eventual stretch
  target includes bare-metal embedded, where there are no threads, no
  syscalls, and no OS-provided mutex/futex — only interrupts that can
  preempt mainline code on the *same* core. This is also what resolves the
  apparent tension between "single-core, no shared memory" and "a mutex at
  all": the mutex isn't for cross-thread contention, it's deliberately just
  an `int` (lock word) plus a pointer to an intrusive list of waiting
  parties — not an OS-backed lock — and its eventual job is guarding a
  `shared_state`'s waiter list against reentrancy from an interrupt/signal
  context. **Revised after M1 first landed** (see that section): the
  hosted-Linux backend has no real interrupts to guard against today, so
  `lock()`/`unlock()` are bookkeeping only for now rather than routed
  through a speculative platform-provided critical section — real
  protection is added when a backend that actually needs it exists.

---

## Toolchain & build system

- **Compiler**: Clang, tracking the 22/23 snapshot line from `apt.llvm.org`
  (no stable Debian/Ubuntu package exists yet for these versions). The
  Dockerfile pins an exact snapshot for reproducibility; bumping it is a
  deliberate, documented action (see Docker section).
- **Standard**: C++23 as the CMake-enforced floor (`cxx_std_23`), with
  C++26 features used opportunistically where Clang trunk supports them and
  where they meaningfully simplify the design (e.g. reflection is *not*
  assumed; this is judged case-by-case, not committed to now).
- **Modules**: the framework is built as a real C++ module, not headers.
  - Primary module interface `est` (`est/src/est.cppm`), composed of
    partitions per component: `est:timer`, `est:sync.mutex`, `est:future`,
    `est:promise`, `est:loop`, `est:coroutine`. The primary interface
    `export import`s each partition so consumers just `import est;`.
  - `import std;` is used inside the framework instead of classic headers,
    gated via CMake's `CXX_MODULE_STD` / `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`.
    This experimental gate's value is tied to the exact CMake version, so
    the Dockerfile pins an exact CMake version too, and
    `cmake/toolchain-hosted-linux.cmake` records which CMake version that
    experimental value was validated against. Bumping CMake in the image
    requires re-validating this in the same commit. **Pinned and verified
    end-to-end** (gate value, Dockerfile packaging fix, and
    `CMAKE_CXX_EXTENSIONS` ordering) — see "import std; re-adopted,
    project-wide" below.
  - Ninja is required (module dependency scanning is Ninja-only in CMake
    today) — the Docker image installs Ninja and CMake is configured to use
    it as the default/only generator for this project.
  - **Fallback posture**: if `import std;` or module support proves too
    unstable on a given Clang snapshot to make forward progress, the
    framework falls back to `#include`-ing specific standard headers inside
    the module interface units (the *framework's own* code stays organized
    as modules either way — only the `import std;` piece is what might get
    deferred). This is a build-system escape hatch, not a design change.
- **Platform abstraction (HAL)**: nothing in the public or internal API may
  hardcode a hosted-OS assumption (POSIX threads, syscalls, an OS-provided
  mutex/futex). A small `est::platform` interface abstracts what the
  framework actually needs from its environment — currently just a
  monotonic time source (for `est::timer_queue`) — rather than writing
  against `<chrono>`'s `steady_clock` directly. A critical-section
  primitive (disable/restore interrupts on bare metal) was in this
  interface during M1's first implementation and was removed: the
  hosted-Linux backend had nothing to guard with it (see M1 section), and
  building it speculatively ahead of a backend that actually needs it was
  premature. It comes back — on `est::platform` or elsewhere — when such
  a backend exists. The only implementation shipped now is the
  hosted-Linux one (used by the dev container and CI); a bare-metal
  implementation is a stretch goal (see
  Roadmap), but designing the seam now is what makes that port later
  realistic instead of a rewrite.
- **Test framework**: Catch2 v3, pulled via `FetchContent` pinned to a
  specific tag. Test binaries are ordinary (non-module) translation units
  that `import est;` and `#include <catch2/catch_test_macros.hpp>` in the
  same TU — mixing is fine since Catch2 itself isn't modularized.
- **Linking**: static by default (`BUILD_SHARED_LIBS` off, not exposed as a
  cache option initially — add later only if a real need shows up).
- **Allocator support**: components that own storage (the future/promise
  shared state, the loop's ready-queue and timer heap, coroutine frames) are
  templated on an allocator type, defaulted to `std::allocator<std::byte>` /
  `std::pmr::polymorphic_allocator<std::byte>` where type erasure at the API
  boundary is preferable (loop-level containers). Coroutine types pick up an
  allocator via the standard "first parameter is `allocator_arg_t`, second
  is the allocator" convention so `promise_type::operator new` can use it.
  This is a cross-cutting concern designed once (as a small `est::allocator`
  concept + helpers) in M1, then reused, not bolted on later.

---

## Repository layout

```
async_experiments/
├── CMakeLists.txt                # top-level: options, subdirs, toolchain checks
├── CMakePresets.json              # "default"/"ci" configure presets ("ci" carries coverage too)
├── cmake/
│   ├── CompilerWarnings.cmake    # shared warning flags for est targets
│   ├── Coverage.cmake            # est_enable_coverage(), the "ci" preset's coverage flags
│   └── toolchain-hosted-linux.cmake  # compiler/stdlib pin, used by the presets
├── docker/
│   └── Dockerfile                # the one image for local dev + CI
├── est/                          # the platform-agnostic framework library
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── est.cppm              # primary module interface (re-exports partitions)
│   │   ├── check.cppm            # est:check — check(), replaces the <cassert> macro
│   │   ├── platform/platform.cppm  # est:platform — hosted-Linux HAL backend
│   │   ├── sync/mutex.cppm       # est:sync.mutex — mutex, mutex_waiter, waiter_list
│   │   ├── timer.cppm            # est:timer
│   │   ├── future.cppm           # est:future — future_state<T>, future<T>
│   │   ├── promise.cppm          # est:promise — promise<T>, make_promise_future()
│   │   └── util/
│   │       ├── scope_exit.cppm   # est:util.scope_exit — RAII "run on scope exit"
│   │       └── shared_ptr.cppm   # est:util.shared_ptr — est::shared_ptr<T>
│   └── tests/
│       ├── CMakeLists.txt
│       ├── platform_tests.cpp
│       ├── check_tests.cpp
│       ├── mutex_tests.cpp
│       ├── timer_tests.cpp
│       ├── future_tests.cpp
│       ├── scope_exit_tests.cpp
│       └── shared_ptr_tests.cpp
├── examples/
│   └── hello_world/
│       ├── CMakeLists.txt
│       └── main.cpp
├── docs/
│   └── PLAN.md                   # this document
├── .clang-format
├── .clang-tidy
└── .github/
    └── workflows/
        ├── devenv-image.yml      # refreshes the ":latest" image tag for local dev
        └── ci.yml                # builds its own SHA-tagged image, then gates on it
```

Component granularity grows as milestones land (`loop.cppm` in M3;
`coroutine.cppm` in M4). This tree is not a frozen contract - the M0
walking-skeleton files it originally showed (`placeholder.cppm`,
`skeleton_tests.cpp`) are gone, replaced by real M1/M2 components as of
this update; expect it to lag again as M3+ land unless someone
remembers to update it.

---

## Docker strategy

One image, `ghcr.io/andijcr/async-experiments-devenv`, used for:
- Local development (mount the repo, get a shell with the exact compiler/
  CMake/Ninja/clang-format/clang-tidy versions CI uses).
- CI: `ci.yml` builds this image itself as the first step of its single
  `gate` job, `--load`ed into the runner's local Docker daemon (never
  pushed to a registry), and drives every subsequent step with `docker
  run` against that local image. This was **not** the original design —
  see "Revised after the first real CI run" and "Adversarial review
  findings" below — but it's what makes the workflow self-sufficient for
  the PR that introduces it, including from a fork.

Built from `docker/Dockerfile`:
1. Slim Debian/Ubuntu base.
2. `apt.llvm.org`'s `llvm.sh` (or manual apt-repo add) to install a
   **pinned** Clang snapshot version.
3. A pinned CMake version with the modules/`import std` experimental gate
   known to work with that Clang snapshot (installed via CMake's own binary
   release archive if the distro package is too old — likely, given how new
   this feature is).
4. Ninja, `clang-format`, `clang-tidy` — both are CI gates from the start,
   not just locally-available tools.
5. A non-root user matching a configurable UID/GID for local bind-mount
   ergonomics.

A separate `devenv-image.yml` workflow refreshes the `:latest` tag in GHCR
(for local `docker pull`ers) once changes have landed on `main`, or on
manual dispatch. It is **not** a dependency of `ci.yml` — see below.

### Revised after the first real CI run

The original design had `ci.yml` only ever `docker pull` a pre-built image,
with a separate `devenv-image.yml` responsible for building and pushing it
on push-to-`main` or manual dispatch. The first real PR (#1, this M0
scaffolding itself) immediately hit the obvious flaw in that split: a
workflow that only builds on push-to-`main` can't produce an image for the
PR that first introduces `docker/Dockerfile` and that workflow, since
neither has landed on `main` yet — and `workflow_dispatch` can't be
triggered for a workflow file that doesn't yet exist on the default branch
either. `ci.yml`'s `docker pull` failed with `manifest unknown`.

First fix: made `ci.yml` self-sufficient by giving it its own `build-image`
job that always builds and pushed the image (tagged by commit SHA), with
a `gate` job `needs:` on it. This also fixed a subtler correctness gap the
original split had: if a PR changed `docker/Dockerfile` itself, `ci.yml`
would have gated against whatever `:latest` happened to already be in the
registry, not the PR's own Dockerfile changes.

That push-based fix was itself replaced shortly after — see "Adversarial
review findings" below — once review turned up that a fork PR's read-only
`GITHUB_TOKEN` would have broken it the same way, just for a narrower set
of PRs. `ci.yml` is now back to one job that builds the image locally
(`--load`, never pushed) and drives it with `docker run`. `devenv-image.yml`
still exists, now purely to keep `:latest` fresh for local `docker pull`
convenience — it is not load-bearing for CI correctness.

### Adversarial review findings (fixed before the toolchain was even pinned)

An adversarial code review of this scaffolding (before any real CI run had
gone green) found three more real bugs, on top of the IPv6 issue above:
1. `clang-tidy` ran *before* `cmake --build` in `ci.yml`. Analyzing a TU
   that `import`s a module needs that module's compiled BMI, which only
   exists after a build — running tidy first fails every PR with `module
   'est' not found`. Reproduced locally, then fixed by reordering (and,
   while restructuring that job — see next point — folded into the same
   `docker run` sequence, now strictly after `build`).
2. `build-image` pushed to GHCR using `packages: write`, but a
   `pull_request` run triggered from a fork always gets a read-only
   `GITHUB_TOKEN` — so `ci.yml` would have silently broken for exactly the
   external-contributor PRs it claimed to be self-sufficient for. Fixed by
   dropping the push-to-registry design entirely for `ci.yml`: it now
   `docker build --load`s the image into the runner's local Docker daemon
   and drives it with plain `docker run` (still cached via
   `cache-from/to: gha`), never needing registry write access. This also
   simplified the job back down to one (`gate`) instead of
   `build-image`+`gate`. `devenv-image.yml` (push-to-`main`/manual-dispatch
   only, always a trusted context) keeps the registry push for local-dev
   `:latest` convenience.
3. The warning flags in `cmake/CompilerWarnings.cmake` had no `-Werror`
   anywhere, and `.clang-tidy`'s checks list never re-enables
   `clang-diagnostic-*` (the family that surfaces plain compiler `-W`
   warnings inside tidy) — so a real `-Wconversion`/`-Wshadow`/etc.
   violation would have merged silently despite `docs/PLAN.md` calling
   the build a hard gate. Fixed by adding `-Werror` to
   `est_set_warnings()`; verified locally that the current placeholder
   code still builds clean with it on.

Investigating fix #1 end-to-end (actually building, not just configuring)
surfaced a further, more fundamental gap the review didn't name directly:
`docker/Dockerfile` installed Clang but never installed libc++'s dev
packages or told Clang to use libc++ over whatever `libstdc++` happens to
be on the system — and this image has no `libstdc++-dev`/`gcc` at all, so
even `<string_view>` could plausibly have failed to resolve, not just
`<print>`. Fixed by installing `libc++-<N>-dev`/`libc++abi-<N>-dev`
alongside Clang. The exact libc++ package names follow apt.llvm.org's
usual convention but, like the Clang/CMake version pins below, could not
be confirmed against a real package listing from this session — verify
when the image is first actually built.

Compiler and stdlib selection (`clang`/`clang++`, `-stdlib=libc++`) was
originally set as CMakeLists.txt logic gated by a CMake option, but moved
into a proper toolchain file, `cmake/toolchain-hosted-linux.cmake`,
applied via the `default`/`ci` presets in `CMakePresets.json` — the
standard CMake mechanism for "which compiler and how", and named
`hosted-linux` so a future bare-metal backend (see Roadmap) gets its own
sibling file instead of this one growing conditionals for a target it
was never meant to describe. Configuring without a preset (as this
session's own local smoke-testing does, lacking libc++) falls back to
CMake's normal compiler search, unaffected by the toolchain file.

### First real toolchain build (Clang 22.1.8 + libc++ - it works)

The `ci.yml` run for the toolchain-file commit was the first to actually
reach the pinned Clang/libc++ combination: `apt.llvm.org`'s IPv4 fix, the
`libc++-<N>-dev` install, and `-stdlib=libc++` from the new toolchain file
all worked together as intended - `est`'s module/partition build succeeded
end to end (`-- The CXX compiler identification is Clang 22.1.8`), which
is the first real confirmation any of this toolchain pinning was correct,
not just plausible.

One new failure surfaced, caused by this session's own `-Werror` fix
(adversarial review finding #3, above): combined with `-Wpedantic`, it
flagged Catch2's own use of `__COUNTER__` inside its `TEST_CASE` macro -
`-Wc2y-extensions`, a diagnostic new enough that it doesn't exist in the
Clang 18 this session used for local verification, so this couldn't have
been caught locally. `__COUNTER__` is long-standing and widely supported
but not yet standard, and Catch2's headers were reaching the compiler via
a plain `-I` rather than as system headers, so `est_tests`' own warning
flags applied to macro expansions from inside them too. Adding `SYSTEM` to `FetchContent_Declare(Catch2 ...)` (CMake >= 3.25)
switched Catch2's include directories to `-isystem` (confirmed in the
next CI run's compile command) but did **not** actually suppress the
diagnostic — Clang still attributed `-Wc2y-extensions` to our translation
unit even with Catch2's headers marked as system. Fixed for real by
disabling the specific diagnostic directly on `est_tests`:
`-Wno-unknown-warning-option -Wno-c2y-extensions` (the first flag first,
so an older/other Clang that doesn't know `-Wc2y-extensions` yet doesn't
turn "unknown warning option" itself into a `-Werror` failure — verified
locally, since this session's Clang 18 is exactly such a case). `SYSTEM`
is kept anyway; it still suppresses every *other* warning class Catch2's
internals would otherwise trip.

That fix got the build all the way through — `est`, `est_tests`, and
`hello_world` all linked successfully, confirming `<print>`/
`std::println` genuinely works against the real Clang 22 + libc++ (the
"Known open items" caveat about this is resolved). One clang-tidy finding
remained: `bugprone-exception-escape` on `hello_world`'s `main()`, since
`std::println` can throw `std::format_error` and nothing caught it.
Fixed with a `try`/`catch (...)` around `main()`'s body — verified the
pattern locally against a stand-in throwing call first (this sandbox's
Clang/libstdc++ has no `<print>` to test the real call directly), and
confirmed a `try`-less version does trigger the same clang-tidy warning
so the test was meaningful.

### Known open items

This session could not reach `apt.llvm.org` or `cmake.org` (network egress
policy for this environment blocks them), so two things are deliberately
left as marked placeholders rather than guessed:
- The exact Clang snapshot version/package name to pin in `docker/Dockerfile`.
- The exact `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` gate value and the minimum
  CMake version it corresponds to, in `cmake/toolchain-hosted-linux.cmake`.

Both are marked `TODO` at their definition site. Filling them in requires a
session/environment with access to those hosts (or the values supplied by
whoever has current documentation open), followed by an actual
`docker build` + in-container configure/build/test to confirm the pin works
before relying on it in CI.

### Docker build verified end-to-end (network-enabled sandbox, post-M2)

A later session's sandbox had real, if unusual, network access: `apt.llvm.org`,
GitHub release downloads (`cmake.org`'s distribution point), and Docker Hub
were all reachable directly (not just from real GitHub Actions), unlike
every prior session recorded in this document. `dockerd` itself needed
starting by hand (`nohup dockerd &`; the packaged `/etc/init.d/docker`
script's `ulimit -Hn`/`ulimit -u unlimited` calls fail under this sandbox's
restrictions, so it has to be invoked directly rather than via `service
docker start`).

`docker build -f docker/Dockerfile .` hit one sandbox-specific snag:
this environment transparently MITMs all outbound TLS (including from
containers, regardless of proxy env vars) through an internal egress
gateway, so `wget`'s fetch of `apt.llvm.org/llvm.sh` failed cert
validation inside the build. This is purely an artifact of the sandbox,
not of the image or of real CI, so it was **not** fixed in the committed
Dockerfile; instead a throwaway local copy added a `COPY`-the-sandbox's-CA
+ `update-ca-certificates` step ahead of the network `RUN`s, verified with
that copy, then discarded.

With that sandbox-only trust issue worked around, the real, repo-relevant
result: `docker build` succeeded outright with the existing pins
(`LLVM_VERSION=22`, `CMAKE_VERSION=3.31.0`) - `Debian clang version
22.1.8`, `cmake version 3.31.0`, and `libc++-22-dev`/`libc++abi-22-dev`/
`libclang-rt-22-dev` all installed clean, matching what real CI had
already found (see "First real toolchain build" above) but now confirmed
independently, from a cold image build rather than relying on a prior
CI run. Running the full CI sequence in a container from that image,
mounted against this repo's actual tree (`--user root`, since the image's
non-root `est` user can't write into a bind mount owned by this sandbox's
root):
- `cmake --preset ci && cmake --build --preset ci` - `est`, `est_tests`
  (Catch2 fetched live via `FetchContent`), and `hello_world` all
  configured, built, and linked with no errors.
- `./hello_world` printed `est::future value: 42` as expected.
- `ctest --preset ci` - 30/30 tests passed.
- `clang-format --dry-run --Werror` over every tracked `.cpp`/`.cppm`/`.h`
  - clean.
- `clang-tidy -p build/ci` over every `.cpp`/`.cppm` - zero warnings
  reported against project code (31544 suppressed warnings were all
  attributed to non-user/system code, per the project's header-filter
  convention).

This is the first time the *entire* documented CI pipeline (image build
through test) has been reproduced independently of GitHub's own runners,
and it confirms the toolchain pins are correct as committed - no
Dockerfile or CMake changes were needed. `docker/Dockerfile`'s two
"package names unconfirmed"/"pin from a blind guess" comments were updated
to reflect this; the `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` situation is
unchanged (that revert was for a real packaging gap - a missing
`std.cppm` - not an unverified pin; see M2's "Round 5" below).

### `import std;` re-adopted, project-wide

Same network-enabled sandbox, immediately following the verification
above. M2's "Round 5" (see above) had tried `import std;` project-wide
once already and reverted it after real CI failed with `CMake Error:
Cannot find source file: /lib/share/libc++/v1/std.cppm` - a concrete but
unexplained packaging gap at the time, since the session that hit it
couldn't reach `apt.llvm.org` to investigate. With real registry/package
access this time, the root cause and a real fix were both found and
verified end-to-end before touching any source file:

**Root cause.** `libc++-22-dev` installs `libc++.modules.json` (the
manifest CMake's `import std;` support reads to find the std module's
sources) twice: once under the versioned
`/usr/lib/llvm-22/lib/libc++.modules.json`, and again at the standard
multiarch path `/usr/lib/x86_64-linux-gnu/libc++.modules.json` - the copy
plain `-stdlib=libc++` actually finds via `clang++
-print-file-name=libc++.modules.json` (confirmed directly: it resolves to
`/lib/x86_64-linux-gnu/libc++.modules.json`, `/lib` being the merged-usr
symlink to `/usr/lib`). Both copies list the same relative
`source-path": "../share/libc++/v1/std.cppm"`, resolved relative to the
JSON's own directory - correct *if* the install prefix were flat (as
upstream LLVM's own release layout is), but Debian's packaging doesn't
mirror `share/libc++/v1/` next to the multiarch lib dir; it only exists
under the versioned `/usr/lib/llvm-22/share/libc++/v1/`. So the relative
resolution lands on `/lib/share/libc++/v1/std.cppm`, which genuinely
doesn't exist - confirmed directly (`std.cppm` only exists at
`/usr/lib/llvm-22/share/libc++/v1/std.cppm`), reproducing the exact
error from the M2 attempt with no guesswork this time.

**Fix.** `docker/Dockerfile` now symlinks
`/usr/lib/share/libc++/v1 -> /usr/lib/llvm-${LLVM_VERSION}/share/libc++/v1`
(a directory symlink, not per-file - `std.cppm`/`std.compat.cppm` both
`#include` sibling `.inc` fragment files from `std/`/`std.compat/`
subdirectories that a per-file symlink would miss, caught by trying the
narrower fix first and watching `clang-scan-deps` fail on
`'std/algorithm.inc' file not found`). This makes the relative path the
broken `libc++.modules.json` actually looks up resolve to the real,
versioned module sources without touching Debian's package contents.

**A second, independent bug surfaced once the first was fixed and
configure succeeded**: the build failed compiling `est`'s own sources
against the newly-available `std.pcm` with `error: GNU extensions was
enabled in precompiled file 'std.pcm' but is currently disabled`. Cause:
`CMAKE_CXX_EXTENSIONS OFF` (and `CMAKE_CXX_STANDARD`/`_STANDARD_REQUIRED`)
were set in the top-level `CMakeLists.txt` *after* `project()` - but
`project()`'s own compiler-detection step is what compiles the internal
`__cmake_cxx23` target (`std.pcm`) that backs `import std;`, using
whatever extensions setting is active at that exact point, not whatever a
later `set()` call says. With no explicit setting yet at that point, it
defaulted to GNU extensions on, while every `est/` target compiled
afterward correctly picked up `EXTENSIONS OFF` and got plain `c++23` -
a real, Clang-enforced configuration mismatch between the two, not a
class of bug specific to this project's code. Fixed by moving those three
`set()` calls to before `project()` in `CMakeLists.txt`, mirroring the
compiler/import-std-gate settings that already had to precede `project()`
in the toolchain file for the same reason. Both bugs were isolated and
confirmed individually in a scratch CMake project before touching the
real repo, then the real fix was verified against `est` itself.

**Migration.** With both bugs fixed, every `.cppm`/`.cpp` file went back
to `import std;` (mirroring the M2 "Round 5" migration this replaces, now
adjusted for `est:check`, which didn't exist at that point): every
`module;`-fragment `#include` of a plain standard header became `import
std;`, except macro-based headers that `import` can never transmit
(`<cassert>` was already gone from `future.cppm` - it now uses
`est::check()` - and `<cstdlib>` stays in `examples/hello_world/main.cpp`
for `EXIT_SUCCESS`/`EXIT_FAILURE`). `est/src/sync/mutex.cppm`'s now-empty
`module;` fragment was dropped entirely.

**Verified end-to-end**, same sandbox, same from-scratch `docker build`
approach as the section above (CA-trust caveat unchanged - sandbox-only,
never committed): `cmake --preset ci` configures cleanly, `cmake --build
--preset ci` builds `est`/`est_tests`/`hello_world` with zero errors,
`hello_world` prints `est::future value: 42`, `ctest --preset ci` is
30/30, `clang-format --dry-run --Werror` is clean, and `clang-tidy` over
every file reports zero findings against project code - identical results
to the `#include`-based build this replaces. The `coverage` preset
(`cmake --build --preset coverage`, `ctest --preset coverage`, the
`llvm-profdata merge`/`llvm-cov export`/`diff-cover` pipeline from
`ci.yml`) was also re-run and completes without error.

The M0-era "import std; unverified" TODO and its M2 "Round 5" revert are
both closed now: the toolchain pins, the packaging-gap symlink, and the
`CMAKE_CXX_EXTENSIONS` ordering are all committed and confirmed against
the real pinned image, not just plausible.

A third, smaller item was flagged the same way and has since been
resolved: `examples/hello_world/main.cpp` uses `std::println` (`<print>`,
C++23) rather than `printf`/`puts`, which could not be locally
build-verified in this environment (its GCC 13 / libstdc++ predates
`<print>`). The real pinned toolchain (Clang 22.1.8 + libc++) has since
confirmed it compiles and links fine — see "First real toolchain build"
above.

---

## CI (`ci.yml`)

Triggers on pull requests (and pushes to `main`). One job, `gate`, run
entirely inside `docker run` invocations of the devenv image (built and
`--load`ed locally in the job's first step, not pushed anywhere — see
"Adversarial review findings" below for why):
1. **Build the devenv image**: `docker build --load` from
   `docker/Dockerfile`, cached via `cache-from/to: gha`.
2. **Format gate**: `clang-format --dry-run --Werror` over all tracked
   `.cpp`/`.cppm`/`.h` files. Any diff fails the check.
3. **Configure**: `cmake --preset ci` (module builds need a generated
   `compile_commands.json`/module map for tidy to work against).
4. **Build**: `cmake --build --preset ci` using Ninja, building `est`,
   its tests, and the `hello_world` example. Runs *before* tidy — see
   "Adversarial review findings" for why the order matters.
5. **Lint gate**: `clang-tidy` over the same file set used for formatting.
   A gate from the very first CI workflow, not deferred — findings fail
   the check same as a format diff.
6. **Test**: `ctest --preset ci`, all Catch2 tests must pass.

This job is a required status check for merging into the default branch
(branch protection is a repo-settings change, not something this plan's
file changes can configure).

### New-code coverage gate (done)

Raised mid-M2: gate CI on *patch/diff* coverage (are the lines a PR adds
actually exercised by a test?), not overall repository percentage.
Implemented self-hosted rather than via a service like Codecov, to keep
everything inside the same self-contained toolchain this project has
otherwise stuck to:
- `cmake/Coverage.cmake`: `EST_ENABLE_COVERAGE` option (default `OFF`,
  so normal builds pay no instrumentation cost) and an
  `est_enable_coverage(target)` helper adding Clang's source-based
  coverage flags (`-fprofile-instr-generate -fcoverage-mapping`) to a
  target's compile and link options. Applied to `est` and `est_tests`.
  When on, also creates `<binaryDir>/profraw/` at configure time —
  `LLVM_PROFILE_FILE` doesn't create its own parent directory and just
  silently fails to write otherwise.
- A new `coverage` CMake preset (`CMakePresets.json`, inherits `ci`,
  `EST_ENABLE_COVERAGE=ON`, `EST_BUILD_EXAMPLES=OFF` — coverage of
  `hello_world` isn't the point), with a matching `coverage` test preset
  that sets `LLVM_PROFILE_FILE` to `<binaryDir>/profraw/%p.profraw` so
  `ctest --preset coverage` (one process per Catch2-discovered test case)
  writes one profile per process without collisions.
- `docker/Dockerfile`: added `libclang-rt-<N>-dev` (compiler-rt's profile
  runtime — what `-fprofile-instr-generate` links against; not reliably
  pulled by `llvm.sh`'s `all`, same situation as libc++), `llvm-cov`/
  `llvm-profdata` added to the unversioned-name `update-alternatives`
  loop, and `diff-cover` (pinned `10.5.1`) via `pip install
  --break-system-packages` — a throwaway container image, not a shared
  system, so overriding Debian's PEP 668 guard is the right call, not a
  workaround.
- `ci.yml`: two new steps in `gate`, after `test` — configure+build+test
  with the `coverage` preset, then merge the resulting `.profraw` files
  (`llvm-profdata merge`), export to lcov (`llvm-cov export`), and gate
  with `diff-cover ... --compare-branch=origin/main --fail-under=80`
  (threshold picked as a reasonable starting point, not derived from
  anything — adjust freely). Needed `fetch-depth: 0` on `actions/
  checkout` (previously the default shallow depth) so `origin/main`'s
  history is actually present for `diff-cover` to diff against.

Verified end-to-end locally (this sandbox's Clang 18 has stable,
long-standing coverage tooling, not a newer-Clang-only feature): every
command in the two new CI steps, run against the exact `coverage` preset
CI uses (temporarily stripping the toolchain file's `-stdlib=libc++`
locally, same workaround used throughout this session for local
verification), correctly produced per-test-case `.profraw` files, merged
and exported to a real lcov report mapping to `future.cppm`/
`promise.cppm`, and `diff-cover` correctly reported 100% coverage of
M2's new code against `origin/main`. `libclang-rt-18-dev` (this
sandbox's equivalent of the pinned image's `libclang-rt-<N>-dev`) had to
be installed to link successfully — confirms that dependency is real,
not just plausible.

### Coverage folded into the main `ci` build (closes a filed optimization issue)

The separate `coverage` preset/CI step above was real but redundant: it
re-ran the *entire* configure+build+test cycle a second time, just to add
two compiler flags, when the plain `ci` preset's own build+test could
carry those flags instead and produce the same `.profraw` as a side
effect of the test run CI was already doing. Folded `EST_ENABLE_COVERAGE=ON`
directly into the `ci` configure preset and moved `LLVM_PROFILE_FILE`
onto the `ci` test preset (pointing at `build/ci/profraw/`); the separate
`coverage` preset and its dedicated `ci.yml` step are gone.
`EST_BUILD_EXAMPLES=OFF` (the old coverage preset's other override,
"coverage of `hello_world` isn't the point") wasn't carried over -
`hello_world` still builds under `ci`, now just also coverage-instrumented
along with everything else; it never runs under `ctest`, so it produces
no `.profraw` and costs nothing beyond a few extra instrumented
instructions in a binary already being built anyway. Net effect: `gate`
drops one full configure+build+test pass without losing any coverage
signal - verified in a from-scratch `docker build` that the single `ci`
build+test still produces `.profraw`, and the merge/export/diff-cover
steps (now reading from `build/ci/` instead of `build/coverage/`) still
run cleanly against it.

### First backlog-issue triage pass

The repo owner filed a batch of GitHub issues (line comments left while reading
the code) - #4 through #17 - covering small correctness/style asks, two
larger design proposals, and two explicitly milestone-gated ideas. Each was
either implemented and verified, or rejected with recorded reasoning
(`gh issue close --reason not_planned` plus a comment); two are left open,
correctly deferred to M3/M4. All code changes below were verified together,
end to end, in the same from-scratch `docker build` this session had already
set up (configure, build, 32/32 tests, `clang-format`, `clang-tidy` - zero
findings - and the coverage gate at 90%).

**Implemented:**
- **#4** (verify `check()` calls are elided in release builds) - they
  weren't, quite: a `-DCMAKE_BUILD_TYPE=Release` build's `objdump`/`nm`
  output showed real `call est::check(...)` instructions still present -
  the `if constexpr (checks_enabled)` inside `check()` only elided its own
  internal branch, not the call site, since Clang's module-BMI visibility
  didn't trigger cross-TU inlining on its own at `-O3`. Marking `check()`
  `inline` fixed it completely - confirmed via the same `objdump`/`nm`
  check that every call site and the symbol itself are now gone.
- **#5** (platform should use `std::print`, not naked `std::cerr <<`;
  everywhere else should stick to `std::format`) - `platform.cppm`'s
  `assert_failure` rewritten to `std::println(stderr, ...)`, the one place
  in the framework that actually performs I/O. Surfaced two follow-on
  findings while doing it: `stderr` (an ordinary extern global in glibc,
  not a macro) still isn't made visible by `import std;` - confirmed via
  the compiler's own diagnostic - so `<cstdio>` stays a plain `#include`;
  and `std::println` can throw (`std::format_error`), which `clang-tidy`
  correctly flagged as an exception escaping a `noexcept` function -
  wrapped in a `try`/`catch (...)` that falls through to the unconditional
  `std::abort()` regardless, same idiom `hello_world/main.cpp` already
  uses for its own `std::println` call.
- **#8** (merge the separate coverage build into the main CI build+test)
  - folded `EST_ENABLE_COVERAGE=ON` and `LLVM_PROFILE_FILE` directly into
    the `ci` preset; removed the now-redundant `coverage` preset and
    `ci.yml` step entirely. One configure+build+test pass now produces
    both the format/tidy-gated binaries and the coverage data.
- **#10** (constrain `scope_exit`'s `Fn` to nothrow-invocable) -
  `~scope_exit()` calls `fn_()` unconditionally, including while another
  exception is already propagating (the guard's whole reason to exist);
  a throwing `fn_` there calls `std::terminate`. Added
  `std::is_nothrow_invocable_v<Fn>` to the template constraint; fixed the
  two call sites (`future.cppm`'s `run()`, `scope_exit_tests.cpp`) whose
  lambdas weren't marked `noexcept` and would otherwise have failed to
  compile against the new constraint.
- **#16** (`future_state::complete` doesn't need to take a functor) -
  `set_value`/`set_exception` now emplace into `result_` directly and call
  a plain, non-template `complete()` that only drains queued
  continuations; the `check(!ready())` precondition moved to each setter
  (it must run *before* that setter's own emplace, so `complete()` itself
  can't perform it - by the time `complete()` runs, `ready()` is
  unconditionally true).
- **#17** (use `std::ranges` + projections, `timer.cppm` and generally) -
  `cancel()`'s `std::find_if` + lambda became `std::ranges::find(entries_,
  target, &entry::timer_id)`; the three heap operations
  (`push_heap`/`pop_heap`/`make_heap`) became their `std::ranges`
  equivalents with `std::ranges::greater{}` + a `&entry::deadline`
  projection, replacing the free `by_deadline_descending` comparator
  entirely. `timer.cppm` was the only file in the project using classic
  `<algorithm>` calls, so the "in general" part of the ask had no other
  call sites to touch.

A real coverage-gate finding came out of #4/#5's rewrite: touching
`platform.cppm`'s previously-untested `assert_failure` body made
`diff-cover` correctly flag the newly-changed lines as uncovered (they
always had been - the gate just never had reason to look at this file
before). Extracted the actual formatting logic (the only part with real
branching - the "is message empty" case) into a separate, exported,
testable `format_assertion_message()`, leaving `assert_failure` itself as
a thin, still-untestable `[[noreturn]]`/`std::abort()` wrapper around it
(same "not practically unit-testable without process-isolation tooling"
situation `check()`'s own failure path already documents). New
`platform_tests.cpp` cases cover both the empty- and non-empty-message
branches; `clang-tidy`'s `readability-container-contains` then correctly
flagged the tests' own `.find(x) != npos` idiom in favor of C++23's
`.contains()`.

**Closed as not planned, with reasoning left on each issue:**
- **#6** (split `platform.cppm` into a declare-only interface + a
  link-time `platform_stdcpp.cppm` implementation, to "remove some
  templating") - doesn't actually remove `timer_queue<Platform>`'s
  template parameter (that exists to swap in `timer_tests.cpp`'s fake
  clock at compile time, orthogonal to where a type's member bodies live)
  and would add a second module partition for two one-line static
  functions with no concrete payoff today - only one platform backend
  exists. Revisit once/if a second backend (the bare-metal stretch goal)
  is actually being built.
  **Reopened and implemented in the issue #30 PR** (see that section's
  "Revised a fifth time" below) once a real payoff appeared: not a second
  *backend*, but `hosted_stdcpp` needing to name `est::loop` for its own
  "current loop" fallback - something `:platform` itself can never do
  without inverting the module dependency DAG. `hosted_stdcpp` moved into
  its own module, `est/src/platform/hosted_stdcpp.cppm`
  (`:platform.hosted_stdcpp`), free to depend on both `:platform` and
  `:loop` precisely because it's no longer `:platform` itself. (Moved
  again in that same PR's "Revised a seventh time" - out of `est`'s own
  module entirely, into a wholly separate one, `estext`.)
- **#11** (`shared_ptr` adopt-a-pointer constructor + a `shared_base`
  mixin for `shared_from_this()` parity) - nothing in the codebase needs
  self-referencing `shared_ptr`s yet; building the mixin now means
  guessing at its shape with no real call site to validate against.
  Revisit when one appears (M3's loop is a plausible candidate).
  **Reopened and implemented in the issue #23 PR** (see that section's
  "Fourth follow-up" below) once a real call site appeared: a review
  comment on that PR wanted `then()`'s wrapped-mode callback to receive
  an `est::future<T>` instead of `future_state<T>&`, which needed exactly
  this - `est::enable_shared_from_this<T>` now exists in
  `est/src/util/shared_ptr.cppm`.
- **#12** (analyze `waiter_list` vs. `std::list`, consider switching) -
  analyzed: the current design is intrusive (the list pointer lives inside
  the already-allocated waiter object, zero extra allocations to enqueue);
  `std::list` is node-based with no small-object optimization, so
  switching would add a mandatory heap allocation per enqueue where there
  is currently none - a straight regression against this project's
  allocator-first design goal, not an improvement, given the current
  design already meets every actual requirement (LIFO push/pop only).

**Left open, acknowledged as correctly milestone-gated (no code change):**
- **#13** (make `mutex` satisfy `std::mutex`'s named requirement) and
  **#15** (`future::get()` suspending the caller when not ready) both
  explicitly depend on `est::loop` (M3) existing, and #15 also needs the
  coroutine adapters (M4) to express the suspension point. Neither exists
  yet - correctly scoped as filed, nothing to implement now.

**Blocked, needs the repo owner:** #9 asked to delete already-merged
branches (`claude/cpp-async-framework-design-jm6ra3` and `poc`, both
confirmed ancestors of `main` via `git merge-base --is-ancestor`) - `git
push --delete` was blocked by this session's own permission classifier as
a destructive action on shared GitHub state. Left for the repo owner to
do directly, or to explicitly authorize.

### `est::platform` becomes a runtime-polymorphic global object

Issue #6 was closed not-planned above because the proposed decl/def split
didn't actually remove `est::timer_queue`'s `Platform` template parameter
and would have broken the fake-platform test seam for no concrete win.
The repo owner came back with a genuinely different design that solves
both problems at once: a virtual `interface` base, exactly one concrete
`final` implementation (`hosted_linux`), and a single globally accessible
instance of it that `est::check()`/`est::timer_queue` call through
directly - no template parameter left at all.

- `est::platform::interface`: pure-virtual `now()` and `assert_failure()`
  - the same two operations M1's stateless-policy-type design exposed as
    static members, now as virtual member functions so a single object
    reference can stand in for "the platform."
- `est::platform::hosted_linux final : interface` — the same
  implementation as before (`std::chrono::steady_clock::now()`;
  `format_assertion_message()` + `std::println(stderr, ...)` +
  `std::abort()`), just as override rather than static members.
- `est::platform::instance()` returns the current global `interface&`
  (backed by a plain `interface*` in a non-exported
  `est::platform::detail` namespace, defaulted to a `hosted_linux`).
  `est::check()` and `est::timer_queue::schedule_after()` call through
  it directly - `timer_queue<Allocator>` has one template parameter now,
  not two.
- `est::platform::override_instance(interface&)` retargets the global for
  the caller's scope, returning an `est::scope_exit` that restores the
  previous instance on destruction - nests correctly. Deliberately built
  on `est::scope_exit` rather than a hand-rolled RAII class once code
  review pointed out `override_guard`'s first draft was reinventing
  exactly what `scope_exit` already exists for.

**The real tradeoff, stated plainly**: this replaces a direct call
(`Platform::now()`, resolved at compile time) with an indirect one
(`platform::instance().now()`, resolved through a vtable) on every
`timer_queue::schedule_after()` and `est::check()` failure. `est::check()`
is unaffected in Release builds specifically - its `if constexpr
(checks_enabled)` branch (and the virtual call inside it) is discarded at
compile time when `NDEBUG` is set, same as before. For `timer_queue`,
the indirect call is now unconditional in both Debug and Release - a
real, deliberate cost taken in exchange for dropping the template
parameter entirely, not a regression that slipped in unnoticed.

**A real bug found by code review before this landed, fixed:**
`namespace detail { ... }` holding the two mutable globals
(`default_instance`, `current_instance`) was originally nested *inside*
the `export namespace est::platform { ... }` block - meaning they were
exported and directly assignable by any `import est;` consumer, bypassing
`override_instance()`'s RAII restore entirely. Fixed by splitting the
file into three blocks: the exported `interface`/`hosted_linux`
definitions, a plain (non-exported) `namespace est::platform::detail
{ ... }` for the two globals, then a second exported block for
`instance()`/`override_instance()` - mirroring `future.cppm`'s own
`est::detail` convention for keeping implementation state out of the
public surface.

**A second, incorrect "fix" from that same review round, caught by a
follow-up review and reverted.** The first round also flagged the
`fake_platform`/`stub_platform` test doubles' `current`/`epoch` fields as
carrying a genuinely-redundant `{}` initializer
(`readability-redundant-member-init`); the fix applied at the time
dropped the `{}` outright, on the assumption that
`std::chrono::time_point`'s default constructor was a *defaulted* one
that would leave an automatic-storage-duration member indeterminate. A
second review pass caught that this reasoning was simply wrong - checked
directly against the pinned toolchain's own `<chrono>` header, `libc++`'s
`time_point()` is a real, *user-provided* constructor
(`time_point() : __d_(duration::zero()) {}`), not a defaulted one, so it
always zero-initializes regardless of storage duration. The linter's
original suggestion was correct all along; reverted to dropping the `{}`
(no `{}`, no NOLINT) once the standard-library source itself settled the
question instead of an assumption about it. Recorded here as a reminder
that "the linter is wrong" needs an actual header/standard citation, not
just a plausible-sounding claim about defaulted-vs-indeterminate
initialization - the same mistake elsewhere would be much harder to catch
than a two-line test fixture.

Verified end-to-end in the same from-scratch `docker build` approach as
the rest of this document's recent entries: `clang-format` clean,
`clang-tidy` zero findings, 34/34 tests (two new ones directly exercising
`override_instance()`'s swap/restore/nesting behavior), coverage gate at
89%.

### `assert_failure()` reworked again: inline prints, `std::cerr` not `stderr`

The repo owner came back with two more concrete asks on top of the
`std::print` migration above: print each field directly instead of
building an intermediate `std::string` first, and use `std::cerr` (an
`ostream`) rather than `stderr` (a `FILE*`). Both landed together:

- `format_assertion_message()` is gone; `assert_failure()` now calls
  `std::print`/`std::println` directly against `std::cerr`, one field at
  a time, inside the same `try`/`catch(...)` as before. Its two dedicated
  unit tests are gone with it - the "is message empty" branch is back to
  being untestable-by-construction, the same situation as `est::check()`'s
  own failure path, since there's no longer a standalone helper to call
  in isolation.
- Switching to `std::cerr` incidentally *removes* the `#include <cstdio>`
  workaround this file needed for `stderr` (documented above, under
  "import std; re-adopted") - `std::cerr` is a genuine `namespace std`
  entity, not a possibly-macro C global, so `import std;` alone reaches
  it.

Two things worth actually checking rather than assuming, given this
project's history of getting exactly this kind of claim wrong (see the
`time_point` default-constructor mistake above): whether removing the
coverage-driving helper regresses the new-code coverage gate, and whether
`std::cerr` genuinely works when reached only via `import std;` - not
just whether it compiles.

- **Coverage**: it doesn't regress the gate. Re-ran `diff-cover` against
  `origin/main` with this change in place (stacked on the still-unmerged
  `est::platform` redesign above, since `diff-cover`'s
  `--compare-branch=origin/main` in `ci.yml` is hardcoded regardless of a
  PR's actual base branch) - 85%, comfortably over the 80% threshold, even
  with `assert_failure()`'s body entirely uncovered again.
- **`std::cerr` via `import std;` alone**: verified with a standalone
  scratch program (not part of this repo) that actually *triggers* the
  path - prints to `std::cerr`, then calls `std::abort()` - rather than
  only compiling and linking it. Captured stdout and stderr separately:
  stdout was empty, stderr contained exactly the expected diagnostic
  line, and the process exited via `SIGABRT` as expected. No
  `#include <iostream>` anywhere in that scratch program either - the
  standard streams' static initialization isn't something `import std;`
  skips.

**The "coverage doesn't regress" claim above turned out to be an
artifact of the stale baseline it was checked against, not a real
result.** It was measured while this change's PR was still stacked on
the not-yet-merged `est::platform` redesign PR, whose own diff's covered
lines diluted the uncovered `assert_failure()` body down to 85%. Once
that PR merged and this one's base moved to point at `main` directly,
`diff-cover` against the *real* current `main` showed the true, isolated
result: 0% - all of `assert_failure()`'s few lines uncovered, none of
the other PR's covered lines left to average against. A follow-up
review comment asked to consolidate the three `std::print`/`std::println`
calls into one (implemented - see the format string above); this didn't
fix the gate either, and if anything made the per-line accounting worse
(each argument expression in a wrapped multi-line call gets its own
coverage region, so the single call spans more counted-but-uncovered
lines than the three short calls it replaced). Per the repo owner's
explicit direction, the coverage gate was not chased further for this
PR - `assert_failure()`'s body stays genuinely untestable-by-construction
without process-isolation tooling (the same situation `est::check()`'s
own failure path has always been in), and building that tooling wasn't
asked for here.

`.clang-format` is based on the LLVM style as a starting point (closest
existing style to how Clang's own modules/coroutines code is formatted, and
clang-format's most battle-tested base for bleeding-edge syntax like module
partitions), with minor project tweaks to be settled as real code
accumulates.

`.clang-tidy` gates CI from M0 onward — not deferred. Starting check set:
`bugprone-*`, `performance-*`, `modernize-*` (tuned to not fight module
syntax), `cppcoreguidelines-*` trimmed to the subset that doesn't conflict
with the allocator-heavy, manual-control-flow style this framework needs
(e.g. owning raw pointers in intrusive lists are expected, not a smell, in
the mutex/waiter-list design below). Exact rule set is tuned as real code
exists to run it against.

### Issue #21: CI cache timing (investigated)

Question raised: can the "build devenv image" step be simplified or
cached for faster CI? Checked with real data instead of guessing — pulled
per-step timing (`started_at`/`completed_at`) for several recent `gate`
job runs via the GitHub Actions API:

| run (Dockerfile touched?) | build-image step duration |
|---|---|
| #25, Dockerfile changed (added the libc++ symlink workaround) | 105s |
| #27, #29, #32, #33, Dockerfile unchanged | 69–70s |

This confirms `cache-from/to: type=gha` is genuinely working — an actual
layer-changing rebuild costs ~35s more than a full cache hit, not a full
from-scratch rebuild (which installs Clang/LLVM/CMake from the network
and takes minutes, per the "Docker build verified end-to-end" timings
earlier in this doc). So the step is already cached; there was no bug to
fix there.

The ~70s that remains even on a full cache hit isn't wasted rebuild work —
it's `docker/build-push-action`'s fixed cost of spinning up buildx,
fetching the cache manifest/blobs from the GHA cache backend, and
`load`ing the resulting image into the runner's local Docker daemon. That
cost is inherent to the "build locally, cache via GHA, never push"
approach this workflow deliberately chose (see the `ci.yml` header
comment and "Adversarial review findings" above) specifically so `pull_request`
runs from forks — which get a read-only `GITHUB_TOKEN` — still work. The
alternatives that would shrink this further (a registry-based cache,
or a job-level `container:` pulling a pre-pushed tag) both need push
(`packages: write`) access and were already rejected for exactly that
reason; re-litigating them would reintroduce the fork-PR breakage this
design exists to avoid.

One real, safe simplification found and applied: `cache-to` was
`type=gha,mode=max`, but `docker/Dockerfile` is single-stage — `mode=max`
only earns its keep by additionally caching intermediate stages that
aren't in the final image, and there are none here. Changed to
`mode=min`, which caches exactly the layers `mode=max` would have cached
in this single-stage case, with slightly less cache-export bookkeeping.
Not expected to be a dramatic win (the `Post build devenv image` step
that writes the cache was already only ~2s), but it's strictly no worse
and more correctly expresses what's actually being cached.

No other change made: the build step's ~70s cache-hit floor is an
accepted, understood cost of a design that already made a deliberate
push-vs-local-cache tradeoff for fork-PR support, not a regression or an
oversight.

---

### Rename: `hosted_linux` → `hosted_stdcpp` (done)

`platform::hosted_linux` (`est/src/platform/platform.cppm`) never actually
did anything Linux-specific — `now()` is `std::chrono::steady_clock::now()`,
`sleep_until()` is `std::this_thread::sleep_until()`, and
`assert_failure()` is `std::println`/`std::abort()`, all pure Standard
Library calls with no `<unistd.h>`/syscall/POSIX dependency anywhere in the
class. The name overstated what the backend actually is: a hosted backend
built entirely on the C++ standard library, not a Linux-specific one.
Renamed the class (and its `default_instance` global, and every comment
referencing it by name) to `hosted_stdcpp` across `platform.cppm`,
`platform_tests.cpp`, `check_tests.cpp`, and `loop_tests.cpp` — a pure
identifier rename, no behavior change.

Deliberately left untouched: the CMake toolchain file
(`cmake/toolchain-hosted-linux.cmake`), `CMakePresets.json`,
`docker/Dockerfile`, and every generic "hosted-Linux"/"hosted Linux" prose
phrase describing the actual deployment target (this project only builds
and tests on Linux right now, which is a true statement independent of
what the C++ class is named) — including `est/src/sync/mutex.cppm`'s own
comment about why its waiter-list protection isn't built speculatively
"into a hosted-Linux backend that has nothing to guard against," the
docs/wiki pages, and every historical mention of `hosted_linux` earlier in
this document, which record what the code was actually called at the time
and aren't rewritten after the fact.

---

### CI gate: AddressSanitizer + UndefinedBehaviorSanitizer unit test build (done)

Added a third build configuration alongside "default" (local dev) and
"ci" (format/tidy/build/test/coverage): a `sanitize` CMake preset that
builds `est`/`est_tests` with `-fsanitize=address,undefined` and runs the
full unit test suite under it, gating PRs the same way the coverage step
already does.

**`cmake/Sanitizers.cmake`** follows `cmake/Coverage.cmake`'s own shape:
an `EST_ENABLE_SANITIZERS` option (default `OFF`, so normal builds pay no
instrumentation cost) and an `est_enable_sanitizers(target)` function
applied to `est` and `est_tests`. Support is checked, not assumed - a
`check_cxx_compiler_flag(-fsanitize=address,undefined ...)` probe at
configure time - so a toolchain whose compiler doesn't accept the flag
gets a clear `message(WARNING ...)` and an uninstrumented build instead
of an obscure link failure or a silently-untested "pass" (this project's
pinned Clang 22 + `libclang-rt-22-dev` supports both sanitizers, so the
gate is live in practice, but the check exists for whatever toolchain
finds this file next). `-fno-sanitize-recover=all` on both targets makes
any single violation - of either sanitizer - abort the process
immediately, so `ctest`'s exit code (and so the CI gate) actually fails
on a finding instead of the run continuing past a printed diagnostic and
reporting green.

`CMakePresets.json` gained a `sanitize` configure/build/test preset:
`CMAKE_BUILD_TYPE=Debug`, `EST_ENABLE_SANITIZERS=ON`,
`EST_BUILD_EXAMPLES=OFF`. Examples are excluded deliberately - the ask
was for a *unit test* sanitizer build, and `hello_world`/`sleep_sort`
aren't run in CI at all, so instrumenting them would only cost build time
for no additional gate coverage. The test preset sets
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` as a second,
belt-and-suspenders layer on top of `-fno-sanitize-recover=all` (the
compiled-in flag already forces an abort; the env vars make that
explicit and add a leak-check pass ASan doesn't run by default).

`.github/workflows/ci.yml`'s existing `gate` job gained three more steps
at the end - configure/build/test against the `sanitize` preset - reusing
the same already-built devenv image rather than adding a second job, the
same reasoning as every other step in that job. This needs its own build
directory (`build/sanitize`, via the preset's own `binaryDir`): coverage
instrumentation (the `ci` preset) and sanitizer instrumentation are
different, mutually-undesirable compile/link flags on the same targets,
so the two configurations can't share one build.

**Verified in the pinned Docker devenv**, beyond the normal build+test
pass:
- `check_cxx_compiler_flag` correctly reports sanitizer support against
  the pinned Clang 22 toolchain (`Performing Test
  EST_COMPILER_SUPPORTS_SANITIZERS - Success`).
- All 68 existing unit tests pass under `-fsanitize=address,undefined` -
  the codebase was already sanitizer-clean, this just makes that an
  enforced property going forward rather than an unverified assumption.
- The gate actually gates: a deliberately introduced heap-buffer-overflow
  (temporary, reverted before committing) made `ctest --preset sanitize`
  report a failed test and a nonzero exit, with ASan's diagnostic
  (allocation site, shadow-byte map) printed to stderr - confirming the
  wiring catches a real bug rather than just building successfully.
- The `ci` preset's own build/test/format/tidy/coverage pass is
  unaffected - `EST_ENABLE_SANITIZERS` defaults `OFF` and is never set
  there, so this is purely additive.

## Roadmap

### M0 — Repo scaffolding (done: this commit)
- Directory layout above.
- Top-level `CMakeLists.txt` wiring together a **trivial** `est` module (one
  partition exporting a placeholder function) + a test that imports it +
  the `hello_world` example calling it — a walking skeleton meant to prove
  the whole pipeline (Docker → CMake configure → Ninja module build →
  Catch2 test → example binary) works end to end before any real framework
  code is written. **Not yet verified against the real pinned toolchain**
  — see "Known open items" above; the module/partition/`FILE_SET
  CXX_MODULES` mechanics were sanity-checked with a local Clang 18 / CMake
  3.28 scratch project (without `import std;`, which needs libc++ modules
  that aren't installed locally), but the actual Docker image build and
  `import std;` path are unverified until the toolchain pins are filled in.
- `docker/Dockerfile`, `.clang-format`, `.clang-tidy`, both GitHub Actions
  workflows (format gate, tidy gate, build, test — all from the start).
- `docs/PLAN.md` (this document).
- No LICENSE file (per requester: skip for now).

### M1 — Foundations: platform seam, timer, mutex (done)
- `est::platform::hosted_linux`: the HAL seam described above, in
  `est/src/platform/platform.cppm` (`est:platform`) — a stateless policy
  type with a single static member, `now()`, not a runtime-polymorphic
  interface. `est::timer_queue` is templated on it (defaulted to
  `hosted_linux`), so tests can substitute a fake and a future bare-metal
  backend is a new template argument, not a redesign. **Superseded** — see
  "`est::platform` becomes a runtime-polymorphic global object" below:
  this whole paragraph describes the M1 design, not the current one.
- `est::timer_queue<Platform, Allocator>`, in `est/src/timer.cppm`
  (`est:timer`) — a `std::vector`-backed binary min-heap of one-shot
  deadlines (`schedule_at`/`schedule_after`, `cancel` — O(n) linear-scan
  cancel, deliberately simple for M1 — `next_deadline`, `pop_ready`),
  templated on the allocator per docs/PLAN.md's allocator-first design.
  No callbacks/continuations yet — that's `est::loop`'s job in M3; this
  is only the scheduling structure a future loop will own and drive.
  **The `Platform` template parameter is gone** as of the same later
  revision noted above — `timer_queue<Allocator>` now calls
  `platform::instance().now()` directly.
- `est::mutex`, in `est/src/sync/mutex.cppm` (`est:sync.mutex`) — exactly
  the `int` lock word (`state_`) plus intrusive singly-linked waiter list
  (`mutex_waiter* waiters_`, LIFO push/pop) the plan called for.
  `lock()`/`unlock()` are bookkeeping-only right now (just toggle
  `state_`, no platform call, no template parameter) — see "Revised:
  critical sections removed" below for why. `mutex_waiter` is
  deliberately payload-free; `est::future`'s `shared_state` (M2) will
  embed one to link into a mutex's waiter list without the mutex needing
  to know what a future is.
- Unit tests under `est/tests/` (`platform_tests.cpp`, `mutex_tests.cpp`,
  `timer_tests.cpp`): a fake platform with a controllable clock for
  deterministic timer-ordering tests; `mutex`'s tests need no fake
  platform at all now.

#### Revised: critical sections removed from `est::platform`

M1's first implementation gave `hosted_linux` `enter_critical_section()`/
`leave_critical_section()` (no-ops), and `est::basic_mutex<Platform>`
routed `lock()`/`unlock()` through them, with a test-only fake platform
whose `enter_critical_section()` could be hooked to simulate a timer/IO
"interrupt" enqueueing its own waiter mid-`lock()`.

The user's call: the design is single-threaded with no shared memory, and
`hosted_linux` had nothing for that critical section to actually protect
— so `est::platform` shouldn't carry that surface area yet. This isn't a
walk-back of the mutex's eventual purpose (still guarding a `shared_state`
waiter list against reentrancy once a backend that can actually be
reentered exists — bare-metal interrupts, or a future multi-loop), just a
deferral of building the protection mechanism before anything needs it
(YAGNI). Resolved via `AskUserQuestion`: kept `lock()`/`unlock()`'s API
shape (so M2 doesn't need to redesign against it later) but dropped the
`Platform` template parameter entirely — `basic_mutex<Platform>` became
plain `est::mutex`, `hosted_linux` lost both critical-section methods, and
the fake-platform-based reentrancy test was deleted (its premise no
longer exists). `est::timer_queue` was unaffected — it only ever used
`Platform::now()`.

**Found while implementing** (all fixed before this landed): `.clang-tidy`
needed one more suppression, `readability-redundant-declaration`,
reproduced in an isolated minimal case — when partition B `import`s
partition A (which pulled in e.g. `<chrono>`) and B *also* independently
`#include`s overlapping standard headers (`<memory>`/`<vector>`, which
transitively reach `<new>` too), this Clang generation's module-aware
analysis sees `operator new`/`delete` declared via two global-module-
fragment paths and flags it as "redundant" — normal, expected behavior
for how C++ modules actually work, not a real duplicate declaration. Any
two interdependent partitions that both reach far enough into the
standard library hit this, so (like `cppcoreguidelines-avoid-do-while`
for Catch2's macros) it's a project-wide suppression with a documented
reason, not a one-off `NOLINT`. Verified locally against Clang 18 (this
sandbox's toolchain, not the pinned Clang 22) — module-aware clang-tidy
analysis is exactly the kind of thing that could plausibly differ on the
real pinned toolchain, so re-confirm this is still needed (and still the
right fix) once M1 runs through real CI.

#### What real CI (and review) actually found, once M1 ran through it

PR #2 was M1's first run through the real pinned toolchain (M0's PR only
ever exercised the placeholder walking skeleton). Two new,
toolchain-specific findings, both fixed and neither reproducible against
this session's local Clang 18 (confirmed: `clang-tidy --list-checks
--checks='*'` on Clang 18 doesn't even know either check by name):
- `readability-redundant-typename` on `est/src/timer.cppm`'s
  `using clock = typename Platform::clock;` (and its two siblings, plus
  the `entry_allocator` alias) — C++20 relaxed where `typename` is
  required in an unambiguously-a-type context like a `using` alias
  declaration (P0634R3), so these were always redundant once the project
  targeted C++23; Clang 18's clang-tidy simply doesn't have the check
  that catches it yet. Fixed by dropping the four now-unnecessary
  `typename` keywords (kept the `template` disambiguator on
  `entry_allocator`'s `rebind_alloc` — clang-tidy didn't flag it, so
  removing it too would be an unverified guess, not a fix).
- `bugprone-throwing-static-initialization` on
  `est/tests/timer_tests.cpp`'s `static inline time_point current{};` —
  libc++'s `steady_clock::time_point` default constructor isn't
  contractually `noexcept` (even though it can't actually throw for an
  arithmetic `Rep`), so a static-storage-duration default-construction of
  one is conservatively flagged as fatal-if-it-threw. Isolated to this
  one line (unlike `readability-redundant-declaration` above, this
  doesn't recur elsewhere), so fixed with a scoped
  `NOLINTNEXTLINE(bugprone-throwing-static-initialization)` rather than a
  project-wide `.clang-tidy` suppression.

Separately, a `code-review` pass caught a real cleanup miss: the
"critical sections removed" commit purged the dead
`enter_critical_section()`/`leave_critical_section()` stubs from
`mutex.cppm`, `platform.cppm`, and `mutex_tests.cpp`'s fake, but missed
the equivalent stubs in `timer_tests.cpp`'s `fake_platform` — `timer_queue`
only ever calls `Platform::now()`, so they were dead, misleading code.
Removed.

### M2 — future / promise / continuation core (done)

The bullets below describe the *final* shape as of commit `d58babd`
(after four review-driven redesign rounds — see the "Found by code
review" / "Round N" retrospective notes further down for how it got
here and why; skip those on a first read, they're history, not current
API surface):

- `future_state<T>`, in `est/src/future.cppm` (`est:future`) — the
  single owned object behind both handles. Holds value-or-exception
  storage (`std::variant<std::monostate, T, std::exception_ptr>`), a
  continuation list built on `est::waiter_list` (its own type, extracted
  from `est::mutex` — see M1's revised section), and a
  `std::pmr::polymorphic_allocator<std::byte>` used to allocate `then()`
  continuation nodes. Holds *no* ref count of its own — lifetime is
  entirely `est::shared_ptr<future_state<T>>`'s job (see below). Not
  templated on a generic `Allocator` the way `est::timer_queue` is:
  `future_state`/`future`/`promise` cross an API boundary (many call
  sites, needs a uniform, non-template-parameterized type), which is
  exactly the case docs/PLAN.md's "Allocator support" section already
  called out for `std::pmr::polymorphic_allocator`-style erasure.
  `get()` uses C++23 deducing-this: called on an lvalue it returns
  `const T&` (non-consuming, safe for multiple readers); called on an
  rvalue (`std::move(state).get()`) it returns `T&&`, an explicit
  opt-in to move, same caveat as `std::optional<T>::value() &&`.
- `est::shared_ptr<T>` (`est:util.shared_ptr`, `est/src/util/shared_ptr.cppm`)
  — a small, general-purpose, single-threaded (plain `int` ref count, no
  atomics) reference-counted pointer, combining the ref count, a
  `std::pmr::polymorphic_allocator<std::byte>`, and the `T` into one
  control block allocated in a single call. `promise<T>`/`future<T>`
  each hold one of `est::shared_ptr<future_state<T>>`.
- Continuations are `est::detail::continuation_node<T>` (`est:future`,
  non-exported — an implementation detail of `then()`, not part of
  `future_state`'s public surface), deriving from the *type-agnostic*
  `est::detail::waiter_node` (`destroy(allocator)` + virtual destructor
  only — nothing `T`-dependent, so it's compiled once instead of once
  per `T`), itself deriving from `est::mutex_waiter` — exactly what
  `mutex_waiter`'s "payload-free at this layer" doc comment in M1
  anticipated: no second allocation for the list node itself. Draining
  (LIFO, same order `est::waiter_list` is documented to use) happens on
  `set_value()`/`set_exception()`; `run()`'s own destroy-on-exit guard is
  `est::scope_exit` (`est:util.scope_exit`), a small reusable RAII "run
  this on scope exit" utility, not an ad-hoc local struct.
- `est::promise<T>`, in `est/src/promise.cppm` (`est:promise`) — producer
  handle: move-only (move ctor/assignment/destructor all `= default`,
  since `est::shared_ptr` itself does the real work), `set_value(const
  T&)`/`set_value(T&&)`/`set_exception`.
- `est::future<T>`, in `est/src/future.cppm` — consumer handle: same
  move-only shape as `promise<T>`. `get()` is deducing-this:
  `future.get()` copy-constructs, `std::move(future).get()` moves — safe
  unconditionally here since a `future` is a single-consumer handle,
  unlike `future_state<T>::get()` itself.
- `future_state<T>::then(fn)` (with `future<T>::then(fn)` a thin
  forwarding call to it — comment #12 in the review round below) runs
  `fn` as `fn(future_state<T>&)`, from which it can `get()`/`ready()`,
  and **returns a `future<U>`** (`U` = `fn`'s return type,
  `static_assert`'d non-`void` for now) so `.then().then()` chains. A
  `fn` exception is caught and routed into that continuation's own
  downstream `future_state<U>` via `set_exception()` instead of
  escaping — isolated per-continuation: a throwing `then()` callback no
  longer stops a sibling continuation queued behind it from running.
  Still runs synchronously on whichever call stack completes the
  `future_state` — there's no loop yet to defer onto (M3 will change
  that).
- `make_promise_future<T>(allocator)`, in `est/src/promise.cppm` —
  builds one `est::shared_ptr<future_state<T>>` and copies it into both
  the `promise<T>` and `future<T>` it returns; the only way a
  `future_state` is created.
- **View semantics**: both handles are thin (an `est::shared_ptr` plus
  nothing else); destroying a `future` does not destroy the
  `future_state` if something else — a still-live `est::promise`, and
  eventually (M3) the loop's own keep-alive registration — still
  references it. Verified directly: dropping a `future` while its
  `promise` is still alive leaves the `future_state` intact and the
  `promise` can still complete it.
- **Abandoned-future semantics — deferred to M3, not a scope cut.** The
  full version (a dropped `future`'s `future_state` is kept alive by the
  *loop's* own reference, not the user's, so the async work keeps
  running in the background) needs `est::loop` to exist to do the
  registering — M2 has no loop yet. What M2 *does* build is the
  ref-counted view mechanics that M3 will hook into: `future_state` isn't
  destroyed while any reference (promise, future, or later the loop's)
  still holds it. The exact "unobserved exception" policy (result/
  exception simply discarded vs. a debug-mode assert or logged warning)
  is still a detail to settle when M3's loop actually implements the
  registration, not now.
- `est::check(condition, message, location)` (`est:check`,
  `est/src/check.cppm`) — a function replacing the `<cassert>` macro,
  used internally by `future_state` for its own precondition checks
  (calls into `platform::hosted_linux::assert_failure()` on failure).
  Not `est::assert`: that name collides with `<cassert>`'s own macro
  even fully-qualified (see the retrospective note below) - a real
  finding worth remembering before naming anything else `assert`,
  anywhere in this codebase.

#### Found by code review, before real CI ever saw it

A `code-review` pass on M2 (before its PR's first CI run) found four real
issues, all fixed:
1. **Wrong-type deallocation (real memory corruption risk).**
   `run()` called `allocator_.delete_object(&node)` on a
   `continuation_node&` — template deduction picks the *base* type, so
   `delete_object` deallocated with `continuation_node`'s size/alignment
   instead of the actual, larger `concrete_continuation<Fn>` that was
   allocated. Silently "worked" with the default new/delete resource
   (sized deallocation isn't always enforced in practice) but is
   undefined behavior per `memory_resource::deallocate`'s contract, and a
   real, visible corruption risk with a pool-style resource —
   `make_promise_future`'s allocator parameter explicitly allows passing
   one. Fixed by giving `continuation_node` a pure-virtual `destroy()`
   that each `concrete_continuation<Fn>` override calls
   `allocator.delete_object(this)` from — `this` is the derived type
   *there*, so the deduction is correct.
2. **Leaked node if a continuation throws.** `run()` had no
   exception-safety around `invoke()`; a throwing continuation skipped
   `delete_object` entirely. Fixed with an RAII guard so `destroy()`
   always runs. The exception still aborts `complete()`'s drain loop
   before any later-queued continuations run — an accepted M2-scope
   limitation (documented in the code), not fixed now: revisit once M3's
   loop dispatches continuations independently instead of inline on the
   completer's call stack.
3. **Value corruption for more than one reader.** `shared_state::get()`
   moved the value out of the `variant` on every call — correct for
   exactly one reader, silently wrong for a second: `ready()` still
   reports true, but the second caller reads a moved-from value. Directly
   contradicts the class's own documented support for multiple `then()`
   registrations. Fixed by making `get()` non-consuming (`const T&`
   instead of `T`); `future<T>::get()` (the single-external-consumer path)
   now copy-constructs its return value instead of moving.
4. **Double-completion guard is debug-only, undocumented as such.** Not a
   bug — `assert(!ready())` matches this project's "validate at
   boundaries, trust internal guarantees" philosophy — but the original
   comment didn't say so. Comment clarified; behavior unchanged (revisit
   only if this turns out to matter in practice, per the project's stated
   philosophy of not adding validation for scenarios speculatively).

While fixing #1/#2, a related leak was found by inspection (not by the
review) and fixed the same way: `~shared_state()` didn't drain its
waiter list, so a continuation registered on a future whose promise is
dropped without ever completing (a "broken promise", or any continuation
still queued when #2's throw aborts the drain) leaked permanently — the
node becomes unreachable once nothing references the `shared_state`
holding its own waiter list. Fixed by draining (destroying, not
invoking) any remaining queued nodes in the destructor.

All four are now regression-tested with a `counting_resource`
(`std::pmr::memory_resource` wrapping the default one, counting
allocate/deallocate calls) — the only practical way to catch a leaked or
wrongly-sized allocation in a unit test without a sanitizer. 19/19 tests
pass locally.

**Found by real CI, not locally reproducible.** PR #3's `clang-format
--dry-run --Werror` step (pinned Clang 22) flagged two spots in
`est/tests/future_tests.cpp` that local Clang 18's `clang-format` accepts
as already-clean — the first confirmed version drift in `clang-format`
itself (previously only `clang-tidy`'s check set and libc++/libstdc++
pairing had shown this kind of gap between the sandbox's Clang 18 and the
pinned Clang 22). Both spots involved a signature/statement sitting right
at the 100-column wrap boundary, where the two versions' line-breaking
heuristics disagree on where (or whether) to break. Rather than guessing
at CI's exact spacing with no way to verify it locally, both were rewritten
to be unambiguous instead: `do_is_equal`'s override shortened well under
the column limit via a local `using std::pmr::memory_resource;` (so
`[[nodiscard]]`, which `clang-tidy` requires, no longer forces a wrap at
all), and a single-line `{ ... }` block with a trailing comment expanded to
the canonical always-multi-line form.

**Found by real CI, round 2 — a genuine bug, not a toolchain-version
drift.** The push that fixed the clang-format issue above turned up a
second, unrelated real-CI-only failure: `est/tests/future_tests.cpp`
calls `std::make_exception_ptr` but only `#include`s `<stdexcept>`, not
`<exception>`. Local libstdc++ (the only stdlib available in this
project's development sandbox) happens to pull `<exception>` in
transitively through `<stdexcept>`; the pinned Clang 22 + libc++ CI
toolchain does not, and `import est;` doesn't re-export the includes
behind `future.cppm`/`promise.cppm`'s own global module fragments to
importers of the `est` module — correct module semantics, and exactly
why this went undetected through every local build. Fixed by adding the
missing include directly.

**A full design-review round from a human reviewer, in parallel with the
above.** Once PR #3 reached real CI, its actual human reviewer worked
through `future.cppm` line by line and left a wave of review comments.
The resulting changes, in the order they landed:
- `est::mutex`'s intrusive waiter-list mechanics were extracted into a
  standalone `est::waiter_list` (`est:sync.mutex`) — `mutex` keeps its
  existing public API, now backed by one. `shared_state<T>` was found to
  be using `mutex_.lock()`/`unlock()` around every waiter-list operation
  for no reason: M2 is entirely synchronous and single-threaded, so
  there was never anything those calls actually protected — real
  protection is already documented (M4 below) as arriving with
  coroutine suspension points, not before. `shared_state<T>` now holds
  an `est::waiter_list` directly, with no lock/unlock calls at all.
- `continuation_node` was split into a type-agnostic `detail::waiter_node`
  base plus a `T`-templated `detail::continuation_node<T>`, and moved out
  of being a nested type of `shared_state<T>` into `est::detail` (see the
  M2 section above) — less code generated per `T`, and no longer part of
  `shared_state`'s public surface.
- `set_value` gained `const T&`/`T&&` overloads (mirroring
  `std::promise`) instead of a single by-value parameter, avoiding an
  extra move on the rvalue path; `promise<T>::set_value` got the matching
  pair so the saving isn't lost one layer up.
- `shared_state<T>::get()` and `future<T>::get()` both became
  deducing-this (see the M2 section above).
- The ad-hoc local `destroy_on_exit` RAII guard in `run()` became
  `est::scope_exit<Fn>` (`est:util.scope_exit`), a small reusable
  "run this callable on scope exit" utility.
- `future<T>`/`promise<T>`'s move-assignment operators became
  swap-based (`std::swap(state_, other.state_); return *this;`) instead
  of `reset()`-then-`std::exchange` — simpler, and self-assignment-safe
  without an explicit check.
- The M0 walking-skeleton scaffold (`est::placeholder_message()`,
  `est/src/placeholder.cppm`, `est/tests/skeleton_tests.cpp`) was
  removed now that real functionality exists to exercise instead;
  `examples/hello_world` was updated to build a promise/future pair and
  print its value.
- One comment (`est::mutex` "satisfying the mutex concept" so
  `std::scoped_lock` could be used) needed no code change: `mutex`
  already has `lock()`/`unlock()`, which already satisfies
  `BasicLockable` — and the mutex-removal change above means
  `shared_state` doesn't hold a `mutex_` to use `std::scoped_lock` on
  any more regardless.

Two more substantial asks from the same review — extracting a
general-purpose `est::shared_ptr<T>` for `shared_state`'s (renamed
`future_state`'s) own ref-counting, and making `then()` return a
chainable `future<U>` — are large enough to be their own follow-up
rounds; see this section's future entries once those land.

**Round 3: `est::shared_ptr<T>`, and `shared_state` renamed to
`future_state`.** New `est::shared_ptr<T>` (`est:util.shared_ptr`,
`est/src/util/shared_ptr.cppm`) — single-threaded (plain `int` ref count,
no atomics), pmr-allocator-backed, combining the ref count, the
allocator, and the `T` into one control block allocated in a single
`allocator.new_object<control_block>` call (no separate control-block
allocation the way `std::shared_ptr` needs one when not built via
`make_shared`). `shared_state<T>` is renamed `future_state<T>` (per the
reviewer's own suggested name) and drops `ref_count_`/`add_ref()`/
`release()` entirely; `promise<T>`/`future<T>` now hold an
`est::shared_ptr<future_state<T>>` instead of a raw pointer plus
hand-rolled ref-counting. A pleasant side effect: `future<T>`/`promise<T>`'s
move constructor/assignment and destructor all became `= default` —
`shared_ptr`'s own move (already swap-based) and destructor do the work
that used to be spelled out by hand in each handle.

**Found by real CI, again — same clang-format wrap-boundary drift as
before, three more times.** `future_state<T>::get()`'s deducing-this
signature (added in round 2, above) sat at the same kind of ambiguous
100-column wrap boundary as the earlier `do_is_equal` case; fixed the
same way, via a named `get_result_t` alias for the (otherwise inline)
`std::conditional_t<...>` return type. The *next* push's real CI then
caught two more instances that a local-Clang-18 check couldn't:
`est::shared_ptr`'s new `make()` signature, and a second copy of the
`do_is_equal` pattern in its own test file's `counting_resource` helper
(the `est/tests/future_tests.cpp` copy had already been fixed this way -
this one was a fresh copy-paste that didn't inherit the fix). Same fix
both times: `make()` got a named `allocator_type` alias; the test helper
got the same `using std::pmr::memory_resource;` treatment as before.
This pattern - any signature landing near the 100-column boundary is a
real risk of CI-only failure regardless of how many times it's been
seen before - is now common enough to just design around from the start
(name the type, keep new signatures well clear of the boundary) rather
than re-discovering it per occurrence.

**Docker was tried as a way to close this gap entirely, and is blocked in
this session's sandbox.** Building the pinned devenv image
(`docker/Dockerfile`) locally, to run the *exact* CI toolchain instead of
guessing at Clang 22's behavior from Clang 18, would eliminate this whole
class of version-drift bug. The Docker daemon itself runs fine here (it
just needed starting), but pulling any base image from Docker Hub is
blocked by this sandbox's network egress policy - reproducible directly
(`docker pull debian:bookworm-slim` fails the same way), not specific to
this Dockerfile. Not a workaround-able failure (403 policy denial, not a
transient error) - noted here as a real limitation of this development
sandbox specifically, not of the project's actual CI, which builds this
same image successfully on every run.

**Round 4: `then()` returns a chained `future<U>`.** `future<T>::then(Fn)`
changed from `void` to `future<U>` (`U` = `Fn`'s return type,
`static_assert`ed non-`void` for now - chaining a `future<void>` isn't
supported yet). The node-allocation-and-registration sequence moved from
`future<T>::then()` into a new `future_state<T>::then()` member (per
review comment #12: "could be hidden in a member function"), which
`future<T>::then()` now just forwards to. Each `then()` callback's
exception, if any, is caught inside the new continuation node and routed
into its own downstream `future_state<U>` via `set_exception()` instead
of escaping - a deliberate, real behavior change from before: a throwing
continuation no longer aborts `complete()`'s drain loop for
later-queued siblings, only its own downstream future observes the
failure. The old "documented M2-scope limitation" test asserting the
opposite (a throw aborts the whole drain) was replaced with two tests:
one proving a sibling continuation still runs after an earlier one
throws, one proving the throwing continuation's own node and downstream
future are still freed, not leaked. A `future_state<T>` forward
declaration of `future<T>` (and vice versa) was needed since
`future_state::then()` returns a `future<U>` but is defined before
`future` in the same partition - ordinary mutual forward declaration,
nothing module-specific.

**Round 5: `import std;`, project-wide - tried, and reverted after a
real-CI finding.** Two changes were meant to make it work:
`cmake/toolchain-hosted-linux.cmake` setting
`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` to `"0e5b6991-d74f-4b3d-a41c-cf096e0b2508"`
(the gate value for CMake 3.30.0-3.31.7, confirmed via WebSearch since
the original scaffolding session couldn't reach cmake.org/discourse to
check; the pinned `CMAKE_VERSION`, 3.31.0, falls inside that range)
before `project()`, and the top-level `CMakeLists.txt` setting
`CMAKE_CXX_MODULE_STD ON` to actually request the module once the gate
allowed it. Every `.cppm`/`.cpp` file's `#include`s became `import
std;`. Locally unverifiable by construction (this sandbox's CMake,
3.28.3, predates the feature outright, and upgrading it isn't possible
either - apt.llvm.org is blocked by the same network egress policy that
blocked Docker Hub earlier, confirmed by trying it directly) - discussed
with the user before proceeding on that basis, and pushed to let real CI
be the only available judge.

Real CI's answer: the gate value and Clang 22.1.8 detection both worked
fine, but configure then failed -
```
CMake Error: Cannot find source file: /lib/share/libc++/v1/std.cppm
```
- the devenv image's installed `libc++-${LLVM_VERSION}-dev` package
doesn't actually ship the standard library module's own source file at
the path CMake expects. `docker/Dockerfile`'s own comment on that
package install already flagged the package names as unconfirmed
(network-blocked when written); this is that risk landing concretely.
Fixing it means finding which apt.llvm.org package (if any, for this
LLVM_VERSION/Debian combination) actually provides `std.cppm` and
adjusting the Dockerfile - not attempted, since apt.llvm.org is blocked
here too and the investigation couldn't be done. Reverted per the
plan's own stated contingency for this outcome: every file back to
`#include`s, `CMAKE_CXX_MODULE_STD` removed from `CMakeLists.txt`,
`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` left commented out in the toolchain
file with the concrete finding recorded in place of the old "couldn't
check" TODO, so a future attempt starts from a real lead instead of
from scratch. Local build verification is fully back as a result - the
"gone for the rest of this session" tradeoff from the initial attempt
did not end up holding.

**One thing from that round survives independently: `est::check()`, a
real function instead of the `<cassert>` macro - requested directly by
the user, not tied to a review comment.** New partition `est:check`
(`est/src/check.cppm`), exported from `est`. Calls into a new
`platform::hosted_linux::assert_failure(message, location)` -
`[[noreturn]]`, prints to `std::cerr`, calls `std::abort()` - the
backend's answer to "what happens when a check fails," same reasoning
as `now()` already being the backend's answer to "what time is it": a
future bare-metal backend answers this differently without
`est::check()` itself changing. `future.cppm` no longer needs
`<cassert>` at all as a result (it already dropped every other classic
include when `import std;` was tried, and kept that reverted back
afterward - simply never needed `<cassert>` again once `check()`
existed).

**Found the hard way: a function cannot be named `assert`, even
namespaced.** The first version of this was named `est::assert()`
directly, per the literal request. It compiles the library itself fine,
but every test file broke: Catch2's headers transitively `#include
<cassert>`, and the C library's `assert` is a function-like macro - the
preprocessor expands any `assert(` token sequence by raw text match
*before* the compiler ever parses the `est::` qualifier in front of it,
so `est::assert(true)` becomes `est::` followed by `<cassert>`'s own
macro expansion, a parse error. Fully-qualifying the call doesn't help;
nothing at the language level can protect a function literally named
`assert` from this in any translation unit that also has `<cassert>` in
scope, transitively or not. Renamed to `check()` (and `assertions_enabled`
to `checks_enabled`, `assert.cppm`/`assert_tests.cpp` to
`check.cppm`/`check_tests.cpp`) to sidestep the collision entirely
rather than working around it per call site.

Two real differences from the macro it replaces, not just a drop-in,
documented directly in `check.cppm`: no automatic condition-
stringification (a function can't see the caller's source text the way
a macro can via `#condition` - pass an explicit message instead of the
old `assert(cond && "message")` idiom), and `condition` is an ordinary
function argument, so it's always *evaluated* even when disabled -
unlike the macro, which under `NDEBUG` never evaluates its argument at
all. `std::source_location::current()`, defaulted at each call site,
replaces `__FILE__`/`__LINE__`. Still debug-only like the macro it
replaces: `checks_enabled` mirrors `NDEBUG`, and the `if constexpr`
around the check compiles it away entirely (not just skips it at
runtime) when set. New `est/tests/check_tests.cpp` covers the
pass-through (condition-true) path only - the failure path unavoidably
terminates the process (`[[noreturn]]`, `std::abort()`), same as the
macro's always did, and isn't practically unit-testable without
process-isolation tooling this project doesn't have.

**Status as of commit `d58babd`: PR #3 (M2 + the new-code coverage gate +
this whole review round) is real-CI green and `mergeable_state: clean`.**
Every review thread on it is resolved. Not yet merged - waiting on the
user (or the next agent, if asked to proceed) to actually merge it; this
session did not merge unilaterally, per the project's own "confirm
before pushing/merging" posture. **Next step once merged:** reset the
`claude/cpp-async-framework-design-jm6ra3` branch onto the post-merge
`main` and start M3 (below) - platform/timer/mutex (M1) and
future/promise/continuation (M2) are both done; nothing is blocking M3
from starting immediately.

### Issue #23: redesigned `then()` chaining, `failed()`, `future<void>` (done)

Requested by the repo owner as a from-scratch redesign of `then()`'s
calling convention, on top of M2's shape described above. `est/src/
future.cppm`/`est/src/promise.cppm` changed; `future_state<T>::then()`'s
own doc comment now describes this directly, so only the *design
decisions* behind it are recorded here.

- **`bool failed()`**, on both `future_state<T>` and `future<T>` —
  `ready() && holds an exception`. Lets a callback that wants the
  wrapped, no-unwrap calling convention (next bullet) check
  success/failure without calling `get()` (which rethrows) just to find
  out.
- **Two calling conventions for `then(fn)`, chosen by how `fn` can be
  invoked** — checked in this order:
  1. `fn(future_state<T>&)` — "wrapped": always called, on success *or*
     failure, with the `future_state` itself; `fn` inspects
     `ready()`/`failed()`/`get()` to decide what to do. This is M2's
     original (only) convention, unchanged.
  2. `fn(const T&)`, or `fn()` when `T` is `void` — "unwrapped": called
     only on success, with the value itself (or nothing, for `void`). On
     failure `fn` is *not* called at all — the returned future fails
     with the same exception instead.
  Checked in that order so a callback explicitly typed to take
  `future_state<T>&` always gets wrapped, no-unwrap behavior, even if it
  would incidentally also accept a `T` (a generic `auto&` lambda, say).
  The unwrapped path's "call only on success, else auto-propagate" isn't
  a separate branch in the implementation — it falls out for free by
  making `get()` (which already rethrows on failure) `fn`'s own argument
  expression: a failure surfaces as an exception thrown *before* `fn`
  ever runs, caught by the same `catch (...)` that already handles `fn`
  throwing on its own.
- **`future<T>::then()`'s callback may now return `void`** (producing a
  `future<void>`) or **a `future<U>`** — the latter is *flattened*: the
  returned future is `future<U>` directly, not `future<future<U>>` a
  caller would have to unwrap again themselves (futures are monadic).
  Implemented by registering a second, internal *wrapped* continuation on
  the inner `future<U>` that forwards its value/exception into the outer
  continuation's own downstream `future_state<U>` once it resolves -
  reusing the exact same get()-rethrows-into-catch propagation idiom, so
  flattening didn't need any new machinery of its own, just one more use
  of `then()`.
- **`future_state<void>`/`future<void>` are not separate class template
  specializations** — `future_state<T>`/`future<T>`/`promise<T>` stay
  single, generic templates, made `void`-safe internally instead. Two
  techniques do the actual work:
  - Methods split by `void`-ness (`set_value()` vs. `set_value(const
    T&)`/`set_value(T&&)`) use a trailing `requires` clause
    (`requires std::is_void_v<T>` / `requires (!std::is_void_v<T>)`) to
    pick the right overload - but the clause alone isn't enough:
    a parameter's *type* is elaborated the moment the enclosing class
    template is instantiated, `requires`-clause or not, so
    `set_value(const T&)`'s parameter would still try to form `const
    void&` (ill-formed) for `future_state<void>` even though that
    overload is constrained out. Fixed by spelling the parameter as
    `const stored_t&` instead of `const T&`, where `stored_t` is `T`
    itself except when `T` is `void`, in which case it's a private
    stand-in tag type (`detail::void_value`) — never actually `void`,
    so the reference is always well-formed, and identical to `T` for
    every other `T` this class was already used with.
  - `get()`'s return type differs three ways (`void`, `const T&`, `T&&`)
    depending on `T`'s void-ness and the deducing-`this` parameter's
    value category. A single `std::conditional_t`-based trailing return
    type (the way `T`/`stored_t` above are picked) doesn't work here:
    `std::conditional_t` instantiates *both* of its type arguments
    unconditionally (unlike `if constexpr`, which discards the untaken
    branch), and the non-`void` alternative names `const void&`/`void&&`
    for `T=void` regardless of which branch would actually be selected
    at runtime. Fixed by dropping the explicit return type entirely
    (deduced `auto`) and spelling each of the three cases under its own
    `if constexpr`, so the ill-formed alternatives are never
    instantiated for `T=void` in the first place. The same
    `conditional_t`-instantiates-both-branches trap applies to computing
    `then()`'s own downstream type from `fn`'s (wrapped- or unwrapped-
    shaped) invoke result — solved the same way, via a small `consteval`
    helper with `if constexpr` branches instead of a `conditional_t`
    expression.
- **A structured-binding gotcha hit while writing the new tests**, worth
  recording since it silently produces a "call to deleted [copy]
  constructor" error that looks unrelated to the actual cause:
  `auto [promise, future] = make_promise_future<T>();` followed later by
  `return future;` (returning a *structured binding name* from a lambda
  building a `future<U>` to flatten into) does **not** get the implicit
  move-on-return that returning an ordinary named local variable would -
  the standard's implicit-move rule is specifically about "the name of
  an object with automatic storage duration declared in the body...of a
  function", which a structured-binding name (an alias into an anonymous
  tuple-like object) doesn't count as. Needs an explicit
  `return std::move(future);`; `future`/`promise` are move-only, so the
  implicit-copy fallback the compiler otherwise tries fails instead of
  silently doing something unwanted.

Verified end-to-end in the from-scratch docker devenv image: configure,
build, 46/46 tests (14 new, covering `failed()`, both calling
conventions and their failure-auto-propagation, `future<void>` end to
end, and both value- and `void`-returning flatten), `clang-format`
clean, `clang-tidy` clean (one real finding along the way -
`readability-redundant-typename` on three `using X = typename Y::type;`
aliases that, per this pinned Clang/libc++ version, don't need the
`typename` disambiguator; removed), new-code coverage 97% (the only
"missing" lines are callback bodies that tests deliberately assert never
run - the whole point of the auto-propagate-on-failure tests).

**Follow-up fix, found by a `code-review` pass on PR #24:**
`future_state<void>::get()` silently "succeeded" (returned normally) when
called before the future was ever completed, in a Release (`NDEBUG`)
build - inconsistent with every other `T`. The precondition
(`ready()`) is checked via `est::check()`, which by design compiles away
entirely under `NDEBUG` (see `check_not_completed()`'s own comment on
this project's validate-at-boundaries philosophy) - that part is
unchanged and correct. What was inconsistent: for `T != void`, `get()`
still went on to call `std::get<T>(result_)`, which throws
`std::bad_variant_access` *unconditionally* (not gated by `NDEBUG`) if
`result_`'s active alternative isn't `T` - an accidental, undocumented
second line of defense. For `T = void`, the live branch was just
`return;`, never touching `result_` at all, so that accidental defense
didn't apply - a caller bug (reading a `future<void>` too early) was
silently masked specifically for `void`, while the identical bug on any
other `future<T>` was at least loudly reported. Fixed by having the
`void` branch also do `(void)std::get<stored_t>(self.result_);` before
returning, so it exercises the exact same (still accidental, still not a
substitute for `check()`) safety net every other `T` already gets.
Not unit-tested: like `check()`'s own failure path and
`platform::hosted_linux::assert_failure()`, this only manifests when
`checks_enabled` is off, which this project's `ctest` binary never
builds with - untestable without process-isolation tooling this project
doesn't have, same accepted gap as those two.

**Second follow-up fix, found by the repo owner's own PR review:** `get()`'s
deduced return type was plain `auto`, not `decltype(auto)` - a real
regression this redesign introduced, not present before it. Plain `auto`
return-type deduction strips references from the return expression's
type (the same rule `auto x = expr;` follows for a local variable), so
`return static_cast<const T&>(std::get<T>(self.result_));` under a plain
`auto` return type silently deduced to `T` *by value*, not `const T&` -
turning the class's own documented "non-consuming `const T&`, safe for
multiple readers" contract into an unconditional copy of `T` on every
call, and (for the rvalue branch) a copy/move outcome subtly different
from the documented "returns `T&&`, an opt-in to move" too. `T = int` in
every existing test meant nothing caught this: a copied `int` and a
referenced `int` compare equal either way. `decltype(auto)` instead
takes the exact type of the return expression, references included -
the same behavior the original, pre-redesign `-> get_result_t<Self>`
explicit trailing return type gave for free, restored without
reintroducing the `std::conditional_t`-instantiates-both-branches trap
`get_result_t` couldn't survive for `T = void` (see above). Verified the
mechanism directly with a standalone reproduction (plain `auto` copies,
`decltype(auto)` doesn't, confirmed by printing from copy/move
constructors) before applying the fix in-repo. Added a regression test
(`future_state::get() returns a reference into the stored value, not a
copy`) that takes `&state.get()` twice and requires the same address -
confirmed it actually catches the regression by temporarily reverting to
plain `auto` first (the address-of an rvalue doesn't even compile, let
alone match) before restoring the fix.

**Third follow-up, a design improvement requested in review:** `then()`'s
`Fn` template parameter was unconstrained; an incompatible callback (one
matching neither calling convention) only failed via a `static_assert`
buried inside the private `raw_result_type_tag()` helper - a correct but
unnecessarily deep diagnostic. Added `detail::then_callback_for<Fn, T>`,
a concept expressing the same "invocable with `future_state<T>&`, or
with `const T&`/nothing for `T=void`" disjunction, and constrained
`then()` with it (`template <detail::then_callback_for<T> Fn> auto
then(Fn&& fn)`), removing the now-redundant `static_assert`s from
`raw_result_type_tag()` (Fn's invocability is already guaranteed by the
time that helper runs). Needed one more instance of the
"can't write `const T&` unconditionally for `T=void`" pattern already
seen twice above in this same file: `std::invocable<Fn&, const T&>`
can't appear directly inside the concept's `||` even when it's the
*second*, seemingly short-circuited operand - `&&`/`||` short-circuit
runtime/constexpr *evaluation*, not the requirement that every
subexpression's types be well-formed to begin with, and forming a
reference to `void` is a hard, non-SFINAE-eligible error regardless of
where it's written. Solved with a small `consteval` helper
(`invocable_unwrapped<Fn, T>()`) using genuinely separate `if constexpr`
branches, the same technique `get()` and `then()` already use, called as
an ordinary boolean-valued expression from inside the concept. Verified
the actual diagnostic improvement directly: a deliberately incompatible
callback (`future.then([](std::string){ return 1; })` on a `future<int>`)
now fails right at the `then()` call site with `error: no matching
member function for call to 'then'` / `note: because
'detail::then_callback_for<..., int>' evaluated to false`, naming the
concept and which branch of it failed - not just a `static_assert`
message from deep inside the implementation.

**Fourth follow-up, reopening issue #11:** a review comment pointed out
that `future_state<T>` is documented as "a detail of `future`" but was
`export`ed and handed directly to a "wrapped" `then()` callback as
`future_state<T>&` - contradicting its own doc comment. Fixing it
properly meant giving `est::shared_ptr<T>` `enable_shared_from_this`
parity (`est/src/util/shared_ptr.cppm`) - exactly issue #11
("shared_ptr adopt-pointer ctor + shared_from_this parity"), closed
earlier this session for lack of a concrete call site; this is one.
Flagged the real cost (a genuine API change, not a local fix) before
doing it, since it meant reopening a design decision the repo owner had
already made once; asked whether to do it in this PR or as a follow-up,
and was told to do it here.

- `est::enable_shared_from_this<T>`: an opt-in CRTP base (mirroring
  `std::enable_shared_from_this`) giving a `shared_ptr<T>`-managed `T`
  a `shared_from_this()` that hands out a *new*, ref-count-bumped
  `shared_ptr<T>` to itself. Weak, not owning, on its own: stores only a
  raw `void*` back-pointer to its own control block, set once by
  `shared_ptr<T>::make()` (via an `if constexpr (requires ...)` check,
  a no-op for any `T` that doesn't inherit from it) right after
  construction - storing an *owning* `shared_ptr<T>` inside `T` itself
  would be a self-cycle this ref-counted pointer's plain `int` count
  could never break (the object would then always hold at least one
  reference to itself). A new `shared_ptr<T>::from_owning_control_block()`
  reconstructs a real `shared_ptr<T>` from that raw pointer, bumping the
  ref count exactly like an ordinary copy would - safe specifically
  because `shared_from_this()` is only ever called while at least one
  other `shared_ptr<T>` to the same object is known to be alive (the one
  whose member function is calling it).
  `bugprone-crtp-constructor-accessibility` (a real clang-tidy finding,
  not a style nit - it flags exactly the classic CRTP mistake of
  inheriting `enable_shared_from_this<Wrong>` instead of
  `enable_shared_from_this<Self>`) made the default constructor private
  with `friend T;`, so only the one correct CRTP usage can construct it
  at all.
- `future_state<T>` now inherits `enable_shared_from_this<future_state<T>>`.
  Its forward declaration moved out of the `export namespace est {}`
  block into a plain `namespace est {}` one - visible to `est:promise`
  (another partition of the same module, which still needs
  `shared_ptr<future_state<T>>`) without being nameable from `import
  est;` consumer code at all anymore, closing the contradiction the
  review comment pointed out. (`est:promise` needed no changes itself -
  cross-partition visibility within one module doesn't require
  `export`, only visibility to code outside the module does.)
- The "wrapped" calling convention's parameter type changed everywhere
  it's checked or used - `detail::then_callback_for`, `then()`'s own
  `raw_result_type_tag()`, `concrete_continuation::invoke()`, and the
  monadic-flatten forwarding lambda in `fulfill()` - from
  `future_state<T>&` to `future<T>&`, built fresh per invocation via
  `state.shared_from_this()`. No change to the *unwrapped* convention or
  to `get()`/`failed()`, which stayed exactly as they were.
- Every existing test's wrapped-mode lambdas (`[](est::future_state<int>&
  state) {...}`) rewritten to take `est::future<int>&` instead - a
  mechanical, if wide, change once the type itself moved. One test
  needed more than a mechanical rename: a regression test added earlier
  this PR directly named `future_state<int>&` specifically to take the
  address of two `get()` calls and prove they matched (verifying
  `decltype(auto)`, not plain `auto`, on the *underlying* `get()`) - no
  longer possible to write that way once `future_state` stopped being
  nameable. Rewritten to observe the same underlying guarantee through
  the still-public *unwrapped* convention instead: two separate `then()`
  registrations, each receiving its own `const int&` argument straight
  from the same `get()`, asserting the two addresses match. Arguably a
  more representative test of a real usage pattern (multiple readers)
  than the original internal-access version was.
- Added direct tests for `enable_shared_from_this` itself in
  `est/tests/shared_ptr_tests.cpp` (a `self_aware` type opting in,
  `shared_from_this()` pointing at the same object, and bumping the ref
  count like an ordinary copy), plus a regression test confirming a `T`
  that doesn't opt in is completely unaffected.

Re-verified the concept-based diagnostic (previous follow-up) still
names the right type after this change: the same deliberately
incompatible callback now fails naming `future<int>&` instead of
`future_state<int>&` in the "candidate ignored" note. Verified end to
end in the docker devenv: 50/50 tests (4 new), `clang-format` clean,
`clang-tidy` clean (one real finding along the way -
`bugprone-crtp-constructor-accessibility` on the new CRTP base, fixed as
described above), new-code coverage 98% against `origin/main`
(`shared_ptr.cppm`'s new code at 100%).

**Fifth follow-up, found by a `code-review` pass on the PR:**
`enable_shared_from_this<T>::shared_from_this()` had no precondition
check at all - calling it on a `T` never actually constructed via
`shared_ptr<T>::make()` (e.g. a stack-allocated or `new`-ed
`self_aware`) dereferenced a null `control_block_` with zero diagnostic.
Every other precondition in this codebase - `future_state::get()`'s
`ready()`, `set_value()`'s `check_not_completed()` - is guarded by
`est::check()`, even if only in debug builds; this one had none, unlike
`std::enable_shared_from_this`, which at least throws `std::bad_weak_ptr`
for the equivalent misuse. Fixed by adding
`check(control_block_ != nullptr, ...)` to `shared_from_this()` (needed
a new `import :check;` in `shared_ptr.cppm`, safe: `:check` already
depends on `:platform`, itself only on `:util.scope_exit`, no cycle with
`:util.shared_ptr`).

Deliberately *not* extended to this file's other bare-pointer operations
(`operator*`/`operator->` on an empty `shared_ptr`) - those match
`std::shared_ptr`'s own long-documented "caller's mistake" contract that
every `shared_ptr` user already knows to avoid, while an
`enable_shared_from_this`-derived `T` built outside `make()` is a much
less obvious, project-specific way to reach the same failure mode. Not
unit-tested: like `check()`'s own failure path elsewhere in this
codebase, this only manifests as an abort with `checks_enabled` on,
which every test here already runs with - untestable without
process-isolation tooling this project doesn't have, same accepted gap.

The same review pass re-surfaced the `fulfill()` monadic-flatten
allocation overhead already reported and discussed earlier on this PR
(see "flattening a chained then() frees every node involved" area of
`future.cppm`) - not a new finding, still not chased per that earlier
discussion.

**Sixth follow-up, a simplification suggested in review:** `get()`'s
lvalue branch explicitly cast its result to `const T&`
(`static_cast<const T&>(std::get<T>(self.result_))`) rather than just
forwarding `self` uniformly and letting `std::get<T>`'s own overload set
pick the reference category - the reviewer suspected the explicit
`const` cast might no longer be necessary but asked to check before
changing it. Traced every call site of `future_state::get()`
(`then()`'s unwrapped dispatch, the flatten forwarding lambda,
`future<T>::get()` itself) - none read the result more than once or
mutate through it; each either discards it immediately (after the
rethrow-on-failure side effect) or copies/forwards it straight into
something else. Confirmed the `const` cast was protecting against a
capability nothing internal to this file ever exercises, and one no
*external* caller can reach either now that `future_state` is
module-private (previous follow-up) - the property `const T&` was
guarding used to matter when `future_state<T>&` was a public `then()`
callback parameter, and stopped mattering once that parameter type
became `future<T>&` instead.

Simplified accordingly: the `if constexpr (std::is_lvalue_reference_v
<Self>)` branch is gone, replaced by a single `return
std::get<T>(std::forward<Self>(self).result_);` - `decltype(auto)`
(already in place from the earlier `auto`-vs-`decltype(auto)` follow-up)
takes whatever `std::get`'s own overload set picks for the forwarded
value category: `T&` for a mutable lvalue, `const T&` for a const
lvalue, `T&&` for an rvalue. A mutable lvalue `get()` call now returns a
genuinely mutable `T&` instead of a forced `const T&` - confirmed safe
specifically *because* of the module-privacy change two follow-ups back,
not in spite of it. Verified end to end: 50/50 tests unchanged,
`clang-format`/`clang-tidy` clean, no call site needed updating.

Two more review comments landed on this PR after the above:

**Seventh, a design question left open:** whether `future<T>::then()`
could pass a `future<T>` clone of itself down into `then()`/the
continuation node, instead of `future_state<T>` needing
`enable_shared_from_this` (the fourth follow-up) at all. Worked through
it: making that actually save anything (not just move the cost)
requires the clone to be conditional on `Fn` needing the wrapped shape,
which means `future<T>::then()` needs its own copy of the
`std::invocable<Fn&, future<T>&>` check (a 5th occurrence of the same
predicate, after the concept, `raw_result_type_tag()`, and `invoke()`),
plus splitting `future_state::then()` into two overloads (with/without
a `future<T>` parameter) sharing most of their body. Weighed against
that: `enable_shared_from_this` costs `future_state` one `void*` (8
bytes) and a vtable-free CRTP base, and `shared_from_this()` itself is
already only ever called lazily, once per *wrapped* continuation that
actually runs - never for unwrapped ones. Posted the trade-off and left
it to the repo owner to decide whether the per-instance bytes are worth
the added overload/duplication; not changed pending their answer.

**Eighth and ninth, a real optimization applied in two places:**
`invoke()`'s unwrapped-mode auto-propagate-on-failure path (`state.get()`,
relying on its rethrow to land in the surrounding `catch (...)`) was
doing an actual C++ throw/catch round-trip purely to retrieve an
exception `failed()` could already tell us was there - the
monadic-flatten forwarding continuation in `fulfill()` had the exact
same pattern (`inner_future.get()`, rethrow into `catch (...)`) for the
same reason. Added `get_exception()` - a plain
`std::get<std::exception_ptr>(result_)`, no rethrow, pairing with
`failed()` - on both `future_state<T>` and (since the flatten lambda
only ever holds a `future<T>&`, never the private `future_state<T>`
directly) `future<T>` itself, and restructured both call sites to check
`failed()` first, calling `get_exception()` for the *known*
propagate-on-failure case instead of relying on `get()`'s throw.
`catch (...)` stays around the remaining, genuinely-uncertain path in
both places (an `fn_` exception in `invoke()`; an unexpected exception
from copying/moving the value itself in the flatten lambda) - only the
"we already know it failed" branch stopped paying for a throw/catch
round-trip it didn't need. `get_exception()` needed one
`NOLINTNEXTLINE(bugprone-exception-escape)`: `std::get` would throw
`std::bad_variant_access` if the (unchecked) precondition were ever
violated, which - escaping this `noexcept` function - terminates
instead of continuing on bad state; intended fail-fast behavior for a
violated precondition, not something to route around.

`get_exception()` is genuinely public on `future<T>` now (not just an
internal `future_state` helper), pairing with the already-public
`failed()` - a deliberate, small, minimal API addition rather than
routing the flatten lambda through friendship, since it's a natural
counterpart to a capability already exposed. Verified end to end: 50/50
tests unchanged, `clang-format`/`clang-tidy` clean, new-code coverage
97% against `origin/main` - the only new gap is the flatten lambda's
`catch (...)` body itself (lines it can now only reach for a genuinely
throwing value copy/move, not the known-failure case that used to also
pass through it), not worth a dedicated throwing-type test for.

### M3 — the looper (done)
- `est::loop`: single-threaded run loop owning the ready-queue and the
  timer min-heap from M1. `run()` drains ready continuations, sleeps until
  the next timer deadline, repeats; `run_until_idle()` for tests/examples
  that shouldn't block forever.
- Owns/creates the `future_state`s it's handed (see M2's abandoned-future
  design) and is the thing that actually resumes continuations when a
  `promise` is fulfilled or a timer fires.
- I/O (sockets, files, epoll/io_uring) is explicitly **out of scope** for
  this initial milestone set — timers are the only external wakeup source
  for now.
- **Long-running-callback detection.** Single-threaded means one
  continuation running too long blocks everything else the loop owns —
  timers, other ready work, all of it — with nothing to preempt it.
  `run()` times each continuation/callback it invokes (`Platform::now()`
  before and after — the clock M1 already built) and logs a warning if it
  ran longer than some threshold, so a runaway handler shows up as a
  clear diagnostic instead of "the whole program mysteriously stalled."
  Threshold value/configurability and exact log destination are details
  to settle when this is actually implemented, not now.

#### Implementation (issue #27, PR pending)

Two foundational design forks were settled with the repo owner via
`AskUserQuestion` before writing any code, since both were expensive to
walk back once dozens of tests and the whole future/promise API were
built around one answer:

1. **Loop wiring: an explicit `loop&` reference, not a global singleton
   like `est::platform::instance()`.** `future_state<T>` now holds a
   `loop&` instead of its own allocator — the allocator it uses is simply
   `loop_.allocator()`. `make_promise_future<T>(allocator)` became
   `make_promise_future<T>(loop&)`. Deliberately not a global-with-
   override-for-tests seam the way `platform` is: unlike platform (a
   stateless vtable swap), a loop carries real mutable state (its
   ready-queue, pending timers) that a shared global would accumulate
   cross-test contamination in — every test constructs its own.
2. **Deferral scope: *all* continuations defer through the loop once one
   exists, not just timer-originated ones.** `future_state<T>::complete()`
   (and `set_continuation()`'s already-ready immediate-run path) now push
   the ready continuation onto the loop's ready-queue instead of invoking
   it inline on the fulfilling call stack — matching this file's own M3
   description above ("is the thing that actually resumes continuations")
   and the M2 section's repeated "M3 will change that" notes. This meant
   rewriting every one of M2/issue #23's ~40 future/promise tests to
   construct a local `est::loop` and call `run_until_idle()` before
   observing a continuation's side effects — a cost flagged as coming,
   not a surprise.

**Avoiding a circular module dependency between `:loop` and
`:future`/`:promise`.** `loop`'s ready-queue needs a type-erased "thing to
run" — naturally `est::future`'s own continuation nodes — but
`future_state<T>` also needs to depend on `loop` to defer onto it, so
`:loop` cannot `import :future`. Resolved by keeping `:loop` the strictly
lower-level partition:
- `est::detail::ready_node` (new, in `:loop`) is the type-erased base for
  anything the ready-queue can hold (`run()` + `destroy(allocator)`) —
  `:future`'s `continuation_node<T>` now inherits it directly instead of
  `:future` defining its own `waiter_node` (which this replaces
  entirely). Dependency direction: `:loop` → `:future` → `:promise`, a
  clean DAG.
- Timer callbacks use the identical intrusive-virtual-node approach
  (`est::detail::timer_node`, `fire()` + `destroy(allocator)`) rather than
  a type-erased `std::move_only_function`, keeping timer-callback storage
  on the same pmr-allocator plumbing already threaded everywhere else in
  this codebase instead of introducing a second, inconsistent allocation
  mechanism.
- `sleep_for()`/`sleep_until()` (the `future<void>`-returning sugar over
  `loop::schedule_timer()`) live in `est/src/promise.cppm` (`:promise`,
  which already imports both `:future` and can import `:loop`) — `:loop`
  itself never names `future`/`promise` anywhere.

**A new self-reference-cycle risk, caught before it shipped, not by a
test.** Once completion defers to the loop instead of running inline, a
continuation node can sit in the ready-queue with nothing external still
referencing its `future_state<T>` — naively "fixed" by having
`continuation_node<T>` hold a permanent `shared_ptr<future_state<T>>`
back-reference (via `shared_from_this()`, from issue #11/#24) instead of
a raw pointer. Tracing it through found a real bug in that first draft: a
node still only *pending* in `future_state<T>`'s own `waiters_` (not yet
ready) is already exclusively owned, by raw pointer, by that same
`future_state` — binding a permanent `shared_ptr` back to it from
construction time on would make the `future_state` count itself as one of
its own owners, keeping an abandoned, never-completed `future_state` (and
its still-pending node) alive forever instead of letting normal
ref-counting destroy it once its promise and future are both dropped.
Fixed by adding `continuation_node<T>::bind_owner()`, called exactly once
— by `complete()`/`set_continuation()`, right before handing the node to
`loop.enqueue_ready()` — instead of at construction: the `shared_ptr` is
only acquired once a node is guaranteed to run soon and is no longer
reachable from the future_state's own `waiters_`, so no cycle exists in
either state.

**`platform::interface` gained a third primitive: `sleep_until()`.**
Mirrors `now()`/`assert_failure()`'s own reasoning — `loop`'s "sleep until
the next timer deadline" step needs a seam a test fake can override to
advance its own fake clock instantly instead of actually blocking, the
same way `est/tests/timer_tests.cpp`'s existing `fake_platform` already
does for `now()`. `hosted_linux::sleep_until()` itself is a plain
`std::this_thread::sleep_until(deadline)`.

**`run()` and `run_until_idle()` are currently identical.** Both drain
ready work, sleep until the next timer deadline, and repeat until truly
idle (no ready work, no pending timers) or `stop()` is called. The
difference this file's own spec anticipates — `run()` blocking
indefinitely, kept alive by a live I/O reactor with more external wakeup
sources than timers — only becomes real once I/O support lands (still
explicitly out of scope); until then nothing could ever wake a fully idle
loop back up anyway (no I/O, single-threaded), so returning is the only
sane behavior for both. Documented as such in `loop.cppm` rather than
pretending a difference that doesn't exist yet.

**Verified in the pinned Docker devenv (`est-devenv:sandbox3`):**
63/63 tests pass (13 new in `est/tests/loop_tests.cpp`, plus 1 new direct
`hosted_linux::sleep_until()` test in `platform_tests.cpp` and 2 fake-
platform stubs updated to implement the new pure-virtual method);
`clang-format --dry-run --Werror` clean (one real formatting fix needed,
caught by the check as expected); `clang-tidy` clean (one real finding:
`cppcoreguidelines-pro-type-static-cast-downcast` on `loop.cppm`'s two
safe-by-construction downcasts from `mutex_waiter*`/`ready_node*`,
suppressed with the same `NOLINTNEXTLINE` reasoning `future.cppm`'s
identical pre-existing downcast already documents); new-code coverage
95% against `origin/main` (comfortably over the 80% gate). The remaining
uncovered lines are all the same kind of "deliberately untestable or
deliberately never executed" gaps this project has consistently declined
to chase elsewhere: fake-platform stub bodies required only to satisfy
`platform::interface` and never actually called by their own test file
(`timer_tests.cpp`'s no-op `sleep_until()`, mirroring `assert_failure()`'s
own long-accepted precedent), an empty `catch (...) {}` around
`loop.cppm`'s best-effort diagnostic `std::println` call (same reasoning
as `hosted_linux::assert_failure()`'s identical catch block), and a
callback body a test explicitly asserts *never runs* (`loop_tests.cpp`'s
`stop()` test — the uncovered line is the point of the test, not a gap in
it).

#### Review follow-ups (PR #28)

Two rounds of review found real, worthwhile fixes, applied on top of the
implementation above:

- **`platform::printdbg()`.** `loop::run_one()`'s long-running-callback
  warning originally formatted and printed for itself, duplicating
  `hosted_linux::assert_failure()`'s own "format, print to `std::cerr`,
  swallow whatever `std::println` itself could throw" pattern. Per
  review, factored into `est::platform::printdbg(std::format_string<Ts...>,
  Ts&&...)` — a plain function template, not a virtual `interface`
  method (C++ has no virtual function templates: a vtable can't have an
  entry per instantiation), so unlike `now()`/`sleep_until()`/
  `assert_failure()` it isn't backend-swappable. `hosted_linux::
  assert_failure()`'s own pre-existing try/catch was deliberately left
  as is (out of scope for what was asked).
- **A standalone code-review pass** (this session's own `code-review`
  skill, verified manually before any fix) found four more issues, all
  fixed:
  1. `future_state<T>`'s bare `loop&` had no documented precondition that
     the loop must outlive everything built against it — an easy first-use
     mistake (e.g. returning a future from a function whose loop is
     local) with no way to check a dangling reference at runtime. Fixed
     by documenting the precondition explicitly on both `future_state<T>`
     (`est:future`) and `loop` (`est:loop`) — there's no code fix possible
     beyond making the requirement impossible to miss.
  2. `loop::schedule_timer()` updated `timers_` and `pending_timers_` in
     two separate steps with no rollback: if the second allocation threw,
     a timer id would exist in `timers_` with no matching
     `pending_timers_` entry, and `fire_ready_timers()`'s `check()` that
     would catch this compiles away entirely under `NDEBUG` — real UB in
     a release build. Fixed by `reserve()`-ing `pending_timers_` *before*
     calling `schedule_at()`: if `reserve()` throws, `schedule_at()` was
     never called (no desync); once it succeeds, the following
     `push_back()` can't reallocate and `pending_entry` is a trivial
     two-member struct, so it can't itself throw.
  3. `run_impl()` unconditionally reset `stop_requested_ = false` on
     entry, so a continuation that called `stop()` and then reentered
     `run()`/`run_until_idle()` on the same loop before returning would
     silently wipe out the still-pending `stop()` request. No coroutine
     machinery exists yet (M4) to make nested pumping a real, supported
     use case, so fixed the same way this codebase treats every other
     precondition it doesn't yet need to actively support: a debug-
     checked `check(!running_, ...)` guard (via a new `running_` member)
     turns silently-wrong behavior into a loud, checked precondition
     violation instead of building real reentrant-`stop()` bookkeeping
     nothing currently needs.
  4. `run_one()` and `fire_ready_timers()` each hand-rolled an identical
     `scope_exit`-based "destroy this node via the loop's allocator no
     matter how we exit" guard. Factored into one `destroy_guard<Node>()`
     helper, returned by value as a genuine prvalue (guaranteed copy
     elision, same as `platform::override_instance()` already relies on)
     despite `scope_exit`'s deleted move constructor. Had to be defined
     *before* `run_one()`/`fire_ready_timers()` in the class body, not
     just declared: a deduced (`auto`) return type needs the function's
     own body resolved before an earlier caller in the same class can use
     it - caught immediately by a real compile error, not a subtle bug.

None of the four got a dedicated regression test: (1) is a documentation-
only fix (nothing to assert on); (2) would need a throwing-on-the-second-
allocation-only test allocator, judged not worth the complexity for an
edge this narrow (same stance PR #24's own flatten-`catch`-body gap
already took); (3)'s failure path is a `check()` abort, untestable-by-
construction without process-isolation tooling this project doesn't have
(same precedent as every other `check()` failure path in this codebase);
(4) is a pure refactor with no behavior change, already covered by every
existing test that exercises `run_one()`/`fire_ready_timers()`. Verified
in the pinned Docker devenv after both rounds: 63/63 tests pass,
`clang-format`/`clang-tidy` clean, new-code coverage 95% against
`origin/main`.

### Refactor: `mutex_waiter`/`waiter_list` generalized into `est::intrusive_list` (done)

Requested by the repo owner directly: `mutex_waiter` was never actually
mutex-specific (just an intrusive `next` pointer), but lived in
`est:sync.mutex` anyway - `:loop` and `:future` each imported that
partition purely to borrow it (as `ready_node`'s/`continuation_node<T>`'s
base and as `waiter_list`'s element type), with zero interest in
`est::mutex`/lock semantics. Moved the node type and its list container to
a new `est:util.intrusive_list` partition, alongside `:util.scope_exit`/
`:util.shared_ptr`:

- `est::intrusive_list_node` (renamed from `mutex_waiter`) and
  `est::intrusive_list<T>` (renamed from `waiter_list`, now a template
  constrained via `std::derived_from<T, intrusive_list_node>`) replace the
  old fixed, `mutex_waiter`-only pair. `est::mutex` keeps `mutex_waiter` as
  a plain alias for `intrusive_list_node` - `est::mutex`'s own public API
  (`enqueue(mutex_waiter&)` etc.) still reads as "a waiting party," not as
  a generic list detail leaking through, and `mutex_tests.cpp` needed zero
  changes.
- **Helpers extracted from the users, per the same request.**
  `intrusive_list<T>::dequeue()` now returns `T*` directly instead of the
  base node type - every call site (`est::mutex`'s own forwarding,
  `est::loop`'s ready-queue, `future_state<T>`'s pending-continuation
  queue) was immediately downcasting the base pointer straight back to
  whatever `T` actually is anyway, each with its own "safe by
  construction, not by RTTI" comment and `NOLINTNEXTLINE
  (cppcoreguidelines-pro-type-static-cast-downcast)`. The cast (and its
  justification) now lives once, inside `dequeue()` itself, not
  duplicated at three call sites. A new `drain(fn)` method extracts the
  identical "dequeue everything left, act on each" loop `est::loop`'s and
  `est::future_state<T>`'s own destructors both needed (destroying an
  abandoned node) and `future_state<T>::complete()` needed (handing every
  pending continuation to `est::loop`) - three copies of the same
  while-loop-plus-cast collapsed into one shared implementation.
  `est::loop::drain_ready()` keeps its own manual loop (unlike the other
  three, it needs to stop partway through to honor `stop()` mid-drain,
  which a generic `drain()` can't support) but still drops its own cast,
  since `dequeue()` now returns the right type directly.
- `est::detail::ready_node` now derives from `est::intrusive_list_node`
  directly instead of from `mutex_waiter` - it was only ever inheriting
  from `mutex_waiter` to borrow the link-field machinery, never because a
  ready-to-run continuation is semantically "a mutex waiter." `:loop` and
  `:future` both dropped their `import :sync.mutex;` entirely, replaced
  with `import :util.intrusive_list;` - a real reduction in coupling, not
  just a rename, since neither ever used anything else from `:sync.mutex`.
- A new `est/tests/intrusive_list_tests.cpp` tests `intrusive_list<T>`
  directly (LIFO order, `dequeue()` returning the derived type with no
  cast needed at the call site, `drain()`'s visit order and end state) -
  the new generalized behavior wasn't exercised by `mutex_tests.cpp`'s own
  indirect use through `est::mutex`.
- The published wiki pages (`docs/wiki/`) were updated in the same
  change: the module dependency graph, the continuation-node type
  hierarchy diagram, and every prose reference to `mutex_waiter`/
  `waiter_list` living in `est:sync.mutex` now reflect the new home and
  names.

Verified in the pinned Docker devenv: 68/68 tests pass (5 new), `clang-
format`/`clang-tidy` clean, new-code coverage 98% against `origin/main`.

### M4 — coroutine adapters (done)

Original plan, as written before implementation:
- `est::task<T>` coroutine type with a `promise_type` that binds to
  `est::promise<T>`/`est::future<T>` under the hood.
- `operator co_await` on `est::future<T>`, suspending into a continuation
  registered on the `future_state`, using symmetric transfer where the
  standard allows it.
- Coroutine frame allocation wired through the same allocator convention
  established in M1/M2 (`allocator_arg_t` + allocator as the coroutine's
  first two parameters, so `promise_type::operator new` can use it).
- **`est::mutex::lock()` becomes awaitable.** A mutex is useful even
  single-threaded: two coroutines writing the same structure across
  multiple steps (e.g. a global registry), with suspension points in
  between, can still interleave and corrupt it — a cooperative-scheduling
  race, not the interrupt-context reentrancy M1's platform seam was
  originally (and no longer is — see M1's "Revised" note) about. The
  `int state_` + intrusive waiter list built in M1 already fits this: the
  gap is that `lock()` today is a synchronous unconditional toggle rather
  than checking `state_` and, if already held, enqueueing the caller's
  `mutex_waiter` and suspending — resumed from `unlock()` like any other
  waiter. Needs coroutine machinery to suspend/resume, so it lands here
  rather than in M1.

#### Implementation

Two design forks, both settled with the repo owner via `AskUserQuestion`
before writing code, changed the plan above in ways expensive to walk back
once tests were built around an answer:

1. **No `est::task<T>`.** The repo owner's own framing: "an homogeneous
   interface — the caller should not know if a computation is built from
   chained futures or a coroutine function." `est::future<T>` itself is now
   the coroutine return type, via a `future<T>::promise_type` nested class
   — see [docs/wiki/Coroutines.md](wiki/Coroutines.md) for the full design.
   This is a real simplification over the planned `task<T>`, not just a
   rename: `promise_type` builds a `shared_ptr<future_state<T>>` directly
   and talks to it through `return_value()`/`return_void()`/
   `unhandled_exception()`, with no intermediate `est::promise<T>` needed
   for the coroutine path at all (`est::promise<T>` is unchanged, still the
   producer type for non-coroutine code like `sleep_for()`).
2. **`est::mutex::lock()` is awaitable-only**, not awaitable-plus-a-
   synchronous-fallback. This is a breaking API change: the old synchronous
   `lock()`/`unlock()` toggle is gone, `mutex` now holds an `est::loop&`
   (needed to defer a waiter's resumption), and every caller must be a
   coroutine using `co_await mutex.lock();`. `mutex_tests.cpp` was rewritten
   in full to drive its scenarios through small coroutines instead of
   calling `lock()` directly, matching the same "existing tests get
   rewritten, flagged as a coming cost, not a surprise" pattern M3's own
   loop-deferral change went through.

The coroutine calling convention settled on: every `est::future<T>`-
returning coroutine takes `est::loop&` as its first parameter (not the
originally-planned `std::allocator_arg_t` + allocator pair) — simpler at
every call site, with the coroutine frame's own allocator derived from
`loop_ref.allocator()` inside `promise_type` rather than threaded through
explicitly. `operator co_await()` on `est::future<T>` was built as planned
(the "using symmetric transfer where the standard allows it" phrasing from
the original plan didn't end up applying in practice — this codebase defers
every resumption through `est::loop`'s ready-queue rather than nested
coroutine-to-coroutine handle transfer, so there's no deep synchronous
resumption chain for symmetric transfer to protect against; see the wiki
page for why).

**A real bug, caught before merging, not after:** the first version of the
coroutine-resumption machinery tried to avoid a heap allocation by having
an awaiter (`initial_suspend()`'s, and `future<T>::operator co_await()`'s)
double as the `ready_node`/`continuation_node<T>` registered with
`est::loop`/`future_state<T>`, reasoning that an awaiter object persists
across its own suspension. That's true only for the duration of *that one*
`co_await` expression — once `run()` resumes the coroutine *past* it, the
compiler is free to reuse that exact frame storage for whatever the
coroutine's later code constructs, and `est::loop::run_one()`'s
`destroy_guard` calls `destroy()` on the same node *after* `run()` already
returned. Reproduced directly as `libc++abi: Pure virtual function called!`
— a virtual dispatch through a vtable pointer already clobbered by the
coroutine's own subsequent frame activity. Fixed by falling back to this
codebase's already-proven pattern: a small, separately heap-allocated
resumption node (`coroutine_resume_node`, `future_resume_node<T>`,
`mutex::lock_resume_node`), exactly like `concrete_continuation<Fn, U>`
and `concrete_timer_node<Fn>` already are, with the frame-embedded awaiter
only responsible for allocating it and never touched again afterward. See
[docs/wiki/Coroutines.md](wiki/Coroutines.md) for the full account — the
ASan+UBSan sanitizer gate added just before this milestone (see its own
section above) confirmed the fix clean, though the bug itself was actually
caught the old-fashioned way first (a debug-build crash under `ctest`),
before sanitizers were even run against this code.

**Verified in the pinned Docker devenv:** 77/77 tests pass (7 new
coroutine tests in `future_tests.cpp` including a leak-check against a
counting `memory_resource`, plus a full rewrite of `mutex_tests.cpp`'s 4
tests to drive `lock()`/`unlock()` through real coroutines, one exercising
LIFO waiter-resumption order); `clang-format`/`clang-tidy` clean (several
real, non-generic findings fixed along the way — deleted copy/move for
every new frame-embedded awaiter type, matching `est::scope_exit`'s own
established "returned as a guaranteed-elided prvalue, never actually
copied/moved" pattern; NOLINT'd the handful of findings that are
either false positives for this codebase's own accepted conventions
(`est::loop&` reference parameters, matching `future_state<T>`/`est::mutex`
already-accepted lifetime-precondition stance) or would have scattered a
single contained finding across every call site instead (making
`await_ready()` `static` moves one `readability-` finding in `future.cppm`
into a `readability-static-accessed-through-instance` finding at every
`co_await` call site instead, since the compiler's own generated code
calls it through an instance regardless — worse, not fixed)); new-code
coverage 99% against `origin/main` (the two lines missed - one
`return_value()` overload never hit by this PR's own tests, one line
inside a coroutine body whose exception path llvm-cov's coroutine-split
function handling doesn't attribute cleanly - not worth chasing given the
overall margin); full suite also passes under the `sanitize` preset
(ASan+UBSan).

#### Code review round (3 bugs found, fixed before merging)

A `/code-review` pass against the M4 PR (matching M3's own "do a code
review" → "fix the automatic code review findings" cycle) found and — after
independent verification against the actual source — fixed three more real
bugs, none caught by the sanitizer pass above (all three are leaks/inline-
execution issues, not memory corruption, so ASan/UBSan had nothing to
flag):

1. **`coroutine_resume_node`/`future_resume_node<T>`/`mutex::lock_resume_node`
   `destroy()` leaked abandoned coroutine frames.** The heap-allocated-node
   fix above solved the crash, but its first `destroy()` only ever freed the
   small trampoline node itself — never `handle_.destroy()` on the coroutine
   frame the handle pointed to. A coroutine abandoned before ever running
   (its owning `future_state<T>`/`loop`/`mutex` torn down while it was still
   only queued, never resumed) leaked its entire frame permanently. Fixed
   with a `ran_`/`invoked_` flag each node now tracks: `destroy()` calls
   `handle_.destroy()` only when `run()`/`invoke()` never actually happened
   — see [docs/wiki/Coroutines.md](wiki/Coroutines.md)'s "Abandoned
   coroutines are destroyed, not leaked" section for the full reasoning
   (importantly, this doesn't need `final_suspend()` to change — it stays
   `std::suspend_never`).
2. **`~mutex()` was simply `= default`**, never draining `waiters_` — a
   coroutine still queued on `lock()` when its `mutex` was destroyed was
   both leaked and left permanently hung (never resumed, never destroyed).
   Fixed by draining `waiters_` in the destructor, the same pattern
   `future_state<T>`/`loop` already use for their own pending queues.
3. **`future_awaiter<T>::await_ready()` returned `future_.ready()`
   directly**, letting an already-ready `co_await` skip suspension and run
   the rest of the awaiting coroutine inline on whatever call stack reached
   it — silently violating this codebase's own tested "never run inline"
   invariant (`future_tests.cpp`'s *"then() registered on an already-ready
   future still defers to the loop"*). Fixed by making `await_ready()`
   unconditionally return `false`; `future_state<T>::set_continuation()`
   already handles the ready-vs-not branching correctly on its own, so
   `await_ready()` doesn't need to and must not special-case it.

Verified in the pinned Docker devenv: 81/81 tests pass (4 new regression
tests — an already-ready `co_await` still deferring, an abandoned
coroutine's frame not leaking, a still-suspended coroutine destroyed with
its awaited `future_state` not leaking, and a mutex destroyed with a
coroutine still queued on `lock()` not leaking — each checking balanced
allocation/deallocation counts against a counting `memory_resource`);
`clang-format`/`clang-tidy` clean; full suite also re-passes under the
`sanitize` preset (ASan+UBSan) after the fix.

#### PR review round: alignment simplification, `mutex::acquire()`

Four more review rounds on the M4 PR itself (PR #37), after it was rebased
onto `main` post-issue-#25/#39/loop-stall-detection:

- **`coroutine_frame_alloc()`/`coroutine_frame_dealloc()`'s alignment**
  simplified from `std::max(alignof(resource_ptr), alignof(std::max_align_t))`
  to `alignof(std::max_align_t)` alone, per a review comment pointing out
  the `std::max` was dead code (the standard guarantees `max_align_t`'s
  alignment already dominates any scalar type's, pointers included).
  `std::min` — the review comment's own first guess — would have been a
  real bug: `align` also stands in for the coroutine frame's own alignment
  requirement, which this code can't query directly, and `std::min` would
  silently under-align any frame needing more than pointer alignment.
- **A regression test added**: a plain `.then()` continuation registered
  directly on a coroutine-returned `future<T>`, exercising the "caller
  can't tell a future came from a coroutine or a then() chain" homogeneity
  through `then()` as well as `co_await`.
- **`initial_suspend()` kept as always-suspend**, after a design
  discussion: the repo owner's instinct was that a coroutine's synchronous
  prefix (up to its first real suspension point) is conceptually the same
  as a plain function's work before returning a future, and so shouldn't
  need to defer through the loop first. The concrete cost of the current
  design is real but narrow: exactly one extra `coroutine_resume_node`
  allocation/deallocation and one ready-queue round trip per coroutine
  call, regardless of how many real suspension points it has (every actual
  `co_await` already allocates its own resume node either way, unaffected
  by this). Weighed against reintroducing the same "might already be
  resolved with side effects applied before the caller ever sees it"
  hazard the `await_ready()` fix (above) deliberately closed for
  `co_await` itself - left as-is for consistency with that fix and with
  `then()`'s own "never run inline" guarantee.
- **`est::mutex::acquire()` added**: `lock()`/`unlock()` stay exactly as
  they were (awaitable-only, manual), and a new `acquire() -> future<
  lock_guard>` sits alongside them - `lock_guard` is a move-only RAII
  handle that calls `unlock()` from its own destructor (suppressed after a
  move), so a caller doesn't have to remember to unlock manually. Not a
  coroutine itself: built directly on `est::promise<lock_guard>`, the same
  "producer without co_await" pattern `sleep_until()` (`est:promise`)
  already uses on top of `est::loop`'s timer queue - `acquire()`'s fast
  path (mutex unlocked) completes the promise immediately, mirroring
  `lock_awaiter::await_ready()`'s own fast path exactly; its slow path
  queues a new `acquire_resume_node` (parallel to `lock_resume_node`, but
  completing a promise instead of resuming a coroutine handle) in the same
  `waiters_` list `lock()`'s own waiters already share.

Verified in the pinned Docker devenv: 93/93 tests pass (7 new `acquire()`
tests in `mutex_tests.cpp` — fast path, RAII unlock on drop, move transfers
ownership, slow path deferring until the holder's guard is dropped,
`co_await`-ability, and a no-leak check against a counting
`memory_resource`); `clang-format`/`clang-tidy` clean; full suite also
passes under the `sanitize` preset (ASan+UBSan).

#### Follow-up: `lock()` converted to `future<void>` too, closing a real leak the conversion itself introduced

A further round of review discussion (same PR) walked back the
"`lock()`/`unlock()` stay exactly as they were" decision above. The repo
owner's own question - "does your analysis change now that the initial
suspend is always false?" - pointed out a real inconsistency: keeping
`lock_awaiter`'s hand-rolled, allocation-free fast path meant `mutex` was
the *one* remaining place in the codebase where "already resolved" could
still skip a genuine suspension, contradicting the very consistency
argument that had just kept `initial_suspend()` always-suspending despite
its own allocation cost. A third option (specializing `future<T>` for a
tag-only `mutex_token`, using `state_ == nullptr` to mean "ready, no
allocation" in the fast case) was raised and rejected: it doesn't remove
the inconsistency, only relocates it into `future<T>`'s own machinery,
for substantially more code than the bespoke awaiter it would replace.
Decision: convert `lock()` to `future<void>`, built exactly like
`acquire()` - accept the allocation cost, in exchange for `future<T>`'s
"never skip a suspend, ever" guarantee holding with no exceptions
anywhere in the codebase. `lock_awaiter` is gone entirely.

**A real bug found while implementing this, before it ever shipped:**
naively swapping `lock_resume_node` from holding a `coroutine_handle<>`
directly to holding a `promise<void>` (matching `acquire_resume_node`'s
existing shape) would have reintroduced a genuine leak - and, worse, a
latent correctness bug - that the existing `acquire()` leak test (added
just above) happened not to catch, because it exercised only a
non-coroutine waiter.

The mechanism: a coroutine doing `co_await mutex.lock();` holds the
resulting `future<void>` as a temporary spilled into its own frame across
the suspension - the *only* other reference to that call's
`future_state<void>`, besides the `lock_resume_node` sitting in
`mutex::waiters_`. The original, handle-holding `lock_resume_node` didn't
have this problem: it held the coroutine handle *directly*, so `~mutex()`
draining `waiters_` was sufficient, on its own, to reach and destroy the
frame. Once the node instead holds a `promise<void>`, matching the
established "broken promise, the future simply never becomes ready"
contract elsewhere in this codebase (e.g. an unfired
`concrete_timer_node`'s own `destroy()`) and just letting the promise
drop silently leaves the `future_state<void>` permanently "not yet
ready," kept alive *only* by the very coroutine frame that can only ever
be freed once that future_state actually completes - neither side can
free the other first. Not a `shared_ptr` cycle in the strict sense, but a
practical one: both the frame and the future_state leak forever. Worse,
had `run()` naively been left able to reference the mutex on a similar
path, resuming a coroutine with a fabricated "you got the lock" success
after the mutex itself no longer exists would have been an active
use-after-free waiting to happen, not just a leak.

Fixed by having `lock_resume_node`/`acquire_resume_node`'s `destroy()`
actually *complete* the promise (with an exception - "mutex destroyed
while lock()/acquire() was pending", the true outcome) when abandoned,
rather than silently dropping it - gated by the same `ran_` flag pattern
`est:future`'s own `coroutine_resume_node`/`future_resume_node<T>` already
use, since `run()` and `destroy()` are always both called, in that order,
for any node the loop actually processes, and completing an
already-completed promise a second time would violate
`future_state<T>::check_not_completed()`. Completing the promise on
abandonment drains the future_state's own pending continuation onto
`est::loop`'s `ready_` queue, where it's either genuinely resumed (if the
loop outlives the mutex and keeps running) or safely destroyed, never
run, by `est::loop`'s own destructor-time drain - either way, the
coroutine frame is no longer stranded. See
[docs/wiki/Coroutines.md](wiki/Coroutines.md)'s own "`lock_resume_node`/
`acquire_resume_node`: completing on abandonment, not just dropping"
section for the full trace.

A new regression test - "destroying a mutex with a coroutine co_await-ing
acquire() still pending leaks nothing" - specifically exercises the gap
the existing, non-coroutine `acquire()` leak test missed; the existing
coroutine-based `lock()` leak test (already in the suite, unmodified)
continues to pass, now for the right reason (verified by deliberately
checking it would *not* have passed without the `destroy()` fix, before
adding it).

**Verified in the pinned Docker devenv:** 94/94 tests pass (1 new); full
suite also passes under the `sanitize` preset (ASan+UBSan) - notably
significant here given how subtle the bug was (a logical "stuck forever"
leak, not a use-after-free ASan would catch on its own; the fix was
verified by hand-tracing every shared_ptr's reference count through the
exact sequence of destructor calls before ever running the test, then
confirming the trace against the actual passing result); `clang-format`/
`clang-tidy` clean. Docs updated: `docs/wiki/Coroutines.md`'s mutex
section rewritten in full, and `docs/wiki/Architecture.md`'s dependency
graph node label.

#### Follow-up: `initial_suspend()`/`await_ready()` reversed - "skip a suspend that isn't needed," not "always defer"

The `initial_suspend()` tradeoff left open above was revisited and settled
the other way, together with `future_awaiter<T>::await_ready()`'s own
review-round fix (a few sections up) - both reversed in the same round,
for the identical reasoning:

- **`promise_type::initial_suspend()`** changed from always suspending
  (via `detail::coroutine_start_awaiter`, deferring even a coroutine's
  first slice of work through `est::loop`) to plain `std::suspend_never`.
  A coroutine's body now runs synchronously, on the caller's own stack, up
  to its first genuine suspension point (or all the way through, for one
  that never awaits anything) - exactly like an ordinary function
  computing a value before handing back a future. `detail::
  coroutine_start_awaiter` and `detail::coroutine_resume_node` are
  removed entirely - nothing else used them.
- **`detail::future_awaiter<T>::await_ready()`** changed from
  unconditionally returning `false` back to `return future_.ready();` -
  `co_await` on an already-ready future now resumes inline immediately,
  no `future_resume_node<T>` allocated and no loop round-trip, instead of
  always genuinely suspending.

The repo owner's framing for reopening this: a coroutine's synchronous
prefix, up to its first real suspension point, is conceptually the same
as the work an ordinary function does before returning a future - and the
rest of the coroutine (after that point) is conceptually the chained
`then()` continuation. Paying a fixed allocation-and-round-trip cost to
defer work that was never actually going to wait for anything stopped
being worth it once weighed that way. This deliberately walks back the
review-round fix from earlier in M4 (`future_awaiter<T>::await_ready()`
returning `future_.ready()` directly was flagged as a real bug then,
specifically because it broke `then()`'s own "never run inline, even when
already ready" invariant) - the two sections aren't in conflict so much as
recording that the tradeoff was judged differently the second time, with
the concrete allocation numbers in hand rather than just the general
principle. `then()` itself keeps its own unconditional-defer invariant
unchanged throughout - only coroutine start/`co_await` moved to the
opposite policy.

Cascading test updates, once the build surfaced exactly what broke:
- Every coroutine test asserting `REQUIRE_FALSE(fut.ready())` (or
  similar) immediately after starting a coroutine with no real suspension
  point had that assertion flipped - the coroutine now completes
  synchronously, so it's already `ready()` by the time the call returns.
- `"co_await on an already-ready future still defers to the loop"`
  (`future_tests.cpp`) was rewritten into `"co_await on an already-ready
  future resumes inline, no loop round-trip needed"`, asserting the
  now-intended opposite behavior.
- `"an abandoned coroutine, destroyed with its loop before ever running,
  leaks nothing"` was deleted outright: its premise (a coroutine started
  but never given a chance to run any of its body before the loop that
  would have resumed it is torn down) is now structurally impossible for
  a `co_await`-free coroutine, since there's no more "deferred, not yet
  started" state for one to be abandoned *in* - it either completes
  synchronously or never runs its first line at all before returning. The
  specific bug it guarded against (`coroutine_resume_node::destroy()`
  freeing only the trampoline node, not the frame) is moot along with the
  now-removed class; the analogous "genuinely suspended, then abandoned"
  scenario remains covered by the very next test, unaffected by any of
  this (`"dropping an awaited future_state destroys the still-suspended
  coroutine, no leak"`).
- `mutex_tests.cpp`'s `"co_await lock() acquires immediately when
  unlocked"` similarly updated - an uncontended `lock()`/`co_await` chain
  is now fully synchronous too, no `run_until_idle()` needed to observe
  either the lock or the coroutine's own completion.

**Verified in the pinned Docker devenv:** 93/93 tests pass (one deleted,
none skipped); `clang-format`/`clang-tidy` clean; full suite also passes
under the `sanitize` preset (ASan+UBSan) - a meaningful check here given
how much of this change is about *when* things run relative to
`est::loop`, exactly the kind of timing-sensitive rework a sanitizer can't
directly catch a wrong-but-not-undefined reordering of, but which did
confirm no lifetime/ownership assumption was broken by any of the
retimed paths. Docs updated: `docs/wiki/Coroutines.md`'s `initial_suspend()`,
`operator co_await`, "abandoned coroutines," and `mutex::lock()` sections
all rewritten to describe the current (and, where relevant, prior)
behavior.

#### Follow-up code review, after the `initial_suspend()`/`await_ready()` reversal above (done, one finding fixed, one finding reverted and documented)

A further `code-review` pass, requested once the `initial_suspend()`/
`await_ready()` reversal (above) and the earlier `mutex::acquire()`/
`lock_guard` addition had both landed on the same branch, found two
issues:

1. **`promise_type::loop_` dead member.** Once `initial_suspend()` no
   longer needed a `loop&` of its own to build a `coroutine_start_awaiter`
   from (removed in the reversal above), nothing in `promise_type` read
   `loop_` again after construction - it was still being stored, unused.
   Fixed by dropping the member and its initializer, keeping the
   constructor parameter (still needed to build `state_`).
2. **Dangling `mutex&` in `acquire_resume_node::run()`.** Confirmed real:
   `unlock()` hands a dequeued waiter to `loop_.enqueue_ready()`, deferring
   its actual `run()` to a later loop drain; if the mutex is destroyed in
   the gap between that hand-off and the drain (possible whenever the loop
   outlives the mutex and keeps running), `run()` constructs a `lock_guard`
   over an already-dangling `mutex&`. The straightforward fix - complete
   the waiter synchronously inside `unlock()` instead of deferring it -
   was implemented, and then reverted after the `sanitize` preset caught a
   *worse* regression it introduced (a stack-use-after-scope in an
   already-established, deliberately-supported "abandoned coroutine"
   scenario). See `docs/wiki/Coroutines.md`'s new "`unlock()`: deferred
   through the loop, not completed inline - and a known, open limitation"
   section for the full trace of both the original hazard and why the
   fix traded it for a different one. Left as a documented precondition
   (loop must outlive every mutex constructed against it; no waiter left
   queued across a mutex's destruction while the loop keeps running)
   rather than "solved" with the flawed fix.

**Verified in the pinned Docker devenv:** 93/93 tests pass; `clang-format`/
`clang-tidy` clean; full suite passes under the `sanitize` preset
(ASan+UBSan) both before attempting the `unlock()` fix and again after
reverting it - the sanitizer is what caught the regression in the first
place, and re-running it after the revert is what confirmed the revert
actually restored the previously-clean state rather than just looking
right on inspection.

### Issue #25: flatten path's throwaway allocation (done)

Found by a `code-review` pass on the issue #23 PR (see that section above)
and deliberately deferred at the time: when a `then()` callback returns a
`future<V>` (monadic flattening), `fulfill()`'s forwarding logic only
needed to *register a callback* on the inner future - but the only way to
do that was `.then()` itself, which always allocates a fresh downstream
`future_state<U>` and `concrete_continuation<Fn, U>` node to hand the
caller a `future<U>` to chain onward from. Nobody chains onward from the
inner forwarding step - `fulfill()` discarded the `future<void>` `.then()`
returned immediately - so that downstream `future_state`/node pair was
pure overhead, allocated and freed for nothing every single flattening
`.then()` call.

The repo owner's own comment on the issue proposed an alternative: "a
special future_state that points to a future, so that once the future is
fulfilled the state can be fulfilled without having to allocate another T,
only a pointer." Evaluated against the issue's own originally-suggested
fix (a lower-level "register a raw callback, no downstream future needed"
primitive) with an eye on template-codegen cost, not just allocation
count:

- The pointer/delegate idea still needs *some* registered callback on the
  inner future to know when to notify the outer future's own waiters - it
  doesn't remove a node, it only changes what the node does (adopt a
  pointer instead of copying a value).
- It adds a second internal representation (own storage vs. delegate to
  another live `future_state`) to every accessor - `ready()`, `failed()`,
  `get()` - of `future_state<T>`, the single most-instantiated type in the
  library (one instantiation per `T` used anywhere, not per `(T, Fn)` or
  `(T, Fn, U)` combination the way continuation nodes are).
- The actual benefit it targets (avoiding a copy of the flattened value)
  was available far more cheaply: the forwarding callback was calling the
  copying `inner_future.get()` instead of `std::move(inner_future).get()`,
  even though `inner_future` is always a fresh, single-owner future
  nobody else can reach.

Settled (repo owner agreed) on the originally-suggested primitive instead,
plus the move fix:

- **`future_state<T>::on_ready(Fn)`**: `then()`'s registration mechanism
  (`set_continuation()`) with the downstream-future half removed - no
  `future_state<U>`, no `future<U>` handed back, nothing to discard. Its
  node, `raw_continuation<Fn>`, is templated on `Fn` alone (not `(Fn, U)`
  like `concrete_continuation`) - no `downstream_` member, no
  wrapped/unwrapped dispatch, no `fulfill()`/`invoke_and_fulfill()`
  recursion baked into its `invoke()`. Exposed on `future<T>` too
  (`future<T>::on_ready()`), forwarding the same way `then()` does.
- `fulfill()`'s flatten branch now calls `on_ready()` instead of `then()`,
  and its forwarding lambda calls `std::move(inner_future).get()` instead
  of `inner_future.get()`.

The move fix turned out to matter beyond performance: forwarding a
move-only value through the old copying path required `U` to be
copy-constructible purely for this internal step, even though nothing
outside `fulfill()` ever touched the copy - a `then()` callback returning
`future<std::unique_ptr<T>>` could not be flattened at all under the old
code (`std::unique_ptr` has no copy constructor for `set_value(const U&)`
to select). A new regression test confirms flattening a
`future<std::unique_ptr<int>>`-returning callback compiles and works
under the fix; it would not have compiled before it.

**Verified in the pinned Docker devenv:** 71/71 tests pass (2 new - the
move-only flatten regression above, plus an exact-allocation-count
assertion added to the existing flatten leak test: 5 allocations for the
worked example in [docs/wiki/Allocation-Patterns.md](wiki/Allocation-Patterns.md),
down from 7 before this fix); `clang-format`/`clang-tidy` clean (one
`bugprone-use-after-move` false positive NOLINT'd - clang-tidy flags
`std::move(x).get()` as a "use after move" of `x` even when it's the
expression's only use); full suite also passes under the `sanitize`
preset (ASan+UBSan). Docs updated:
[docs/wiki/Continuation-Node-Mechanism.md](wiki/Continuation-Node-Mechanism.md)'s
"Flattening is not a special case" section and
[docs/wiki/Allocation-Patterns.md](wiki/Allocation-Patterns.md)'s
flattening-cost section, both rewritten for the new allocation count and
`on_ready()` mechanism.

#### Follow-up: `on_ready()` degeneralized to `detail::flatten_forwarder<T>` (done)

The repo owner's own follow-up review of the above: "if we believe that
`on_ready` is only a private abstraction built to support a continuation
returning a function, then we should: ensure that it's not a public method
of `future`, degeneralize and pass the raw_continuation node only the
downstream_copy, skipping the lambda altogether."

Correct on both counts. `on_ready()` had exactly one real caller
(`fulfill()`'s flatten branch) with one fixed shape of forwarding logic -
templating its node on an arbitrary `Fn` and exposing it publicly on both
`future_state<T>` and `future<T>` was generality nothing needed, paid for
with a fresh node type (and lambda closure type) per `(T, Fn, U)` call
site instead of one shared instantiation per inner value type `T`.

Fixed:

- **`future<T>::on_ready()` and `future_state<T>::on_ready()` removed
  entirely**, along with the nested `raw_continuation<Fn>` class.
- **`detail::flatten_forwarder<T>`** added in its place: a free class
  template in `est::detail`, alongside `detail::continuation_node<T>` -
  not nested inside `future_state<T>`, since it needs nothing private to
  `future_state<T>` beyond what `future_state<T>`'s own public interface
  (`failed()`, `get_exception()`, `get()`, `set_value()`,
  `set_exception()`) already exposes. Templated on the inner value type
  alone, with the forwarding logic (fail → propagate exception; succeed →
  move the value or catch-and-propagate) hardcoded directly into
  `invoke()` - no `Fn` member, no closure, no `future<T>` view built via
  `shared_from_this()` (the earlier design's `on_ready()`/wrapped shape
  needed that view only to hand a generic callback something to call;
  `flatten_forwarder<T>` has no callback to call, so it never needs one).
- **`fulfill()`'s flatten branch** now allocates a `flatten_forwarder<U>`
  directly and registers it via `result.state_->set_continuation(*node)`,
  reaching `result`'s private `state_` (a `future<U>`) through a new
  `template <class> friend class future_state;` declaration on
  `future<T>` - nested-class members share their enclosing class's access
  rights, so this one friend declaration on `future<T>` is all every
  `future_state<T>` instantiation's nested `concrete_continuation` needs,
  not one per `(T, U)` pair.

No observable behavior changed - same allocation count, same move (not
copy) of the flattened value - only the mechanism: a lower-codegen,
non-public node replacing a public, per-closure-type primitive that only
ever had one legitimate caller.

**Verified in the pinned Docker devenv:** full test suite still passes
unchanged (the flatten tests, including the `std::unique_ptr` move-only
regression and the exact-allocation-count assertion, exercise `.then()`'s
observable behavior, not `on_ready()` directly, so none needed updating);
`clang-format`/`clang-tidy` clean; full suite also passes under the
`sanitize` preset (ASan+UBSan). Docs updated:
[docs/wiki/Continuation-Node-Mechanism.md](wiki/Continuation-Node-Mechanism.md)'s
flattening section (now covering all three iterations: `.then()`,
`on_ready()`, `flatten_forwarder<T>`) and
[docs/wiki/Allocation-Patterns.md](wiki/Allocation-Patterns.md)'s
flattening-cost section.

### Issue #39: `then()`'s wrapped-vs-unwrapped precedence, flipped (done)

Reported by the repo owner against issue #23's original design (see that
section above): `then()` checked the *wrapped* shape (`Fn` invocable with
`future<T>&`) before the *unwrapped* shape (`Fn` invocable with `const T&`,
or with nothing for `T = void`). For an `Fn` explicitly typed one way or
the other this made no difference, but a **generic** callback — an `auto&`
or `auto&&` lambda, incidentally invocable both ways, since a template
parameter binds to either — silently got wrapped, no-unwrap behavior
without ever opting into it. The repo owner's framing: "an auto lambda
should be interpreted as T, not future<T>. T should take precedence over
future<T> (which should be an explicit decision by the user)."

Fixed by swapping the check order in the two places `future_state<T>::then()`
dispatches on it — `raw_result_type_tag()` (computing `Fn`'s pre-flatten
result type) and `concrete_continuation<Fn, U>::invoke()` (actually calling
`Fn`) — to check `detail::invocable_unwrapped<Fn, T>()` first, falling
through to the wrapped shape only when unwrapped isn't viable. An `Fn`
explicitly typed to take `future<T>&` still gets wrapped behavior exactly
as before (it isn't invocable with a plain `const T&`, so unwrapped was
never viable for it to begin with) — only a callback generic enough to
accept both shapes changes behavior, now defaulting to unwrapped. Getting
wrapped behavior deliberately still requires writing it explicitly
(`[](est::future<T>& state) { ... }`), exactly the "explicit decision by
the user" the repo owner asked for.

`then_callback_for<Fn, T>` (the concept constraining `then()`'s Fn
parameter) needed no change — `std::invocable<Fn&, future<T>&> ||
invocable_unwrapped<Fn, T>()`'s `||` doesn't care about operand order, only
whether at least one side holds.

Two new regression tests in `future_tests.cpp` exercise a generic `auto&`
lambda through both the success and failure paths, checking it now gets
unwrapped semantics (called with the plain value on success; skipped, not
invoked, with the exception auto-propagated on failure) — the same
observable behavior an explicitly `const T&`-typed callback already had,
now also the default for a generic one.

A related, harder issue found while writing regression tests for this fix:
`then_callback_for<Fn, T>`'s own disjunction, `std::invocable<Fn&,
future<T>&> || invocable_unwrapped<Fn, T>()`, needed the same reordering,
and not just for consistency. Constraint disjunction (`||`) short-circuits
left-to-right, so whichever operand is written first is the one actually
evaluated for an `Fn` that satisfies it. A generic callback that's only
*valid* when called with a plain `T` - e.g. one that returns a copy of its
argument, which `future<T>`'s deleted copy constructor makes ill-formed
for a `future<T>&` argument - hard-errors, not a graceful SFINAE failure,
if `std::invocable<Fn&, future<T>&>` is even instantiated for it ("use of
a deleted function" isn't overload-resolution failure). With the wrapped
check written first in the concept, such a callback failed to compile at
all, constraint or no constraint. Reordering the concept's own disjunction
to check `invocable_unwrapped<Fn, T>()` first fixes this the same way it
fixes the dispatch: satisfied there, the wrapped check is never even
attempted.

Verified in the pinned Docker devenv: 70/70 tests pass (2 new);
`clang-format`/`clang-tidy` clean; full suite also passes under the
`sanitize` preset (ASan+UBSan).

### Refactor: loop-stall detection moved to `platform::interface` (done)

Requested by the repo owner: `loop::run_one()`'s long-running-callback
detection (M3, "the looper" section above) measured a node's runtime
itself, bracketing `node.run()` with two `platform::instance().now()`
calls and printing straight from `run_one()` if the gap exceeded
`long_running_threshold`. Moved onto `platform::interface` instead, as two
new methods:

- **`reset_loop_stall_detection()`** — called immediately before running a
  node, marking the start of a fresh measurement window.
- **`detect_loop_stall(std::chrono::steady_clock::duration threshold)`** —
  called immediately after, checking whether the gap since the matching
  `reset_loop_stall_detection()` call exceeded `threshold` and reporting a
  diagnostic via `platform::printdbg()` if so.

`run_one()` itself shrinks to just the two bracketing calls plus
`node.run()` in between — the actual timing/measuring/printing is now
`platform::interface`'s job, for the same reason `now()`/`sleep_until()`
are already platform hooks rather than `est::loop` calling
`std::chrono`/`std::this_thread` directly: "how do we know a callback ran
long" is a policy question a backend should get to answer for itself. The
repo owner's own framing: today's synchronous, measure-after-the-fact
approach is the only strategy that makes sense for a single-threaded
backend, but a future backend could use a background thread to detect a
stall in parallel — watching for one *while* the callback is still
running, rather than only finding out once it returns — without
`run_one()`'s own call site changing at all.

Unlike `now()`/`sleep_until()`/`assert_failure()` (pure virtual - every
backend must answer for itself), the two new methods are virtual *with a
default body* in `interface` itself: the default just records `now()` on
reset and compares against it on detect, printing via `platform::
printdbg()` — exactly the old `run_one()` logic, relocated rather than
rewritten. This means every existing concrete `interface` — `hosted_stdcpp`
and every test fake in `est/tests/` (`fake_platform`/`jumping_platform` in
`loop_tests.cpp`, `fake_platform` in `timer_tests.cpp`, `stub_platform` in
`platform_tests.cpp`) — inherits working stall detection for free, with
zero changes needed to any of them; only a backend that actually wants
different behavior (the hypothetical background-thread one) would need to
override both. `printdbg()` itself moved earlier in `platform.cppm` (before
`class interface`, instead of down near `override_instance()`) purely so
`detect_loop_stall()`'s default body can call it — no behavior change.

`loop_tests.cpp`'s existing `jumping_platform`-based test (exploiting a
`now()` that advances on every call to make a continuation's timing appear
to exceed the threshold without a real delay) needed no changes to keep
passing: the default `detect_loop_stall()` still calls `now()` exactly
twice bracketing `node.run()` (once via `reset_loop_stall_detection()`,
once via `detect_loop_stall()` itself), the same two-calls-bracketing-run()
shape `run_one()` used to have directly.

Verified in the pinned Docker devenv: full test suite passes unchanged
(no test additions or removals - the existing long-running-callback test
already exercises this path end-to-end, platform-relocated or not);
`clang-format`/`clang-tidy` clean; full suite also passes under the
`sanitize` preset (ASan+UBSan).

#### Review follow-up: `printdbg()` made backend-swappable too

A PR review comment on the refactor above pointed out an inconsistency it
introduced: `printdbg()` itself was left exactly as it always had been -
hardcoded to `std::println(std::cerr, ...)`, with its own doc comment
explicitly noting "this can't be swapped per backend... every backend gets
the same `std::cerr` behavior" - even though the diagnostic it exists for
(`detect_loop_stall()`'s default body) had just become backend-swappable
one call up. Suggested fix: split `printdbg()` the same way
`std::print()` splits from `std::vprint_unicode()` - formatting stays a
template (`std::format_string<Ts...>` needs the caller's own argument
types for its compile-time check), but *where the formatted text goes*
becomes a new `interface` method.

- **`interface::vprintdbg(std::string_view fmt, std::format_args args)`** -
  pure virtual, unlike `reset_loop_stall_detection()`/`detect_loop_stall()`
  above: "measure wall-clock time" has one sensible default nearly any
  backend can share, but "where does a debug line go" doesn't - a
  bare-metal target may have no console at all. `hosted_stdcpp`'s override
  calls `std::vprint_unicode(std::cerr, fmt, args)` (swallowing whatever it
  throws, same as `assert_failure()`'s own `std::println` call already
  does).
- **`printdbg()`** now does its compile-time-checked formatting, then
  hands the format string and `std::make_format_args(args...)` (not
  `std::forward`'d - `make_format_args()` itself takes plain lvalue
  references, and forwarding an rvalue argument here would bind that
  reference to a temporary about to expire) to
  `platform::instance().vprintdbg(...)`.

Because `vprintdbg()` is pure virtual, every existing test fake needed one
new override - unlike `reset_loop_stall_detection()`/`detect_loop_stall()`,
which every fake inherited for free. Three (`fake_platform` in
`timer_tests.cpp`, `fake_platform`/`jumping_platform` in `loop_tests.cpp`)
just discard the message, matching the established no-op-override
convention for a method nothing in that file needs to actually observe
(`sleep_until()`'s own no-op precedent in `timer_tests.cpp`).
`platform_tests.cpp`'s `stub_platform` instead *records* the formatted
message (via `std::vformat(fmt, args)`), specifically so a new test -
"`printdbg()` dispatches through the currently overridden instance" -
can confirm the whole point of this change: `printdbg()` is now a genuine
part of the `override_instance()`-swappable seam, not a fixed behavior
every backend was stuck with.

Verified in the pinned Docker devenv: 71/71 tests pass (1 new); `clang-
format`/`clang-tidy` clean (one `cppcoreguidelines-missing-std-forward`
false positive NOLINT'd, for the deliberate non-forwarding described
above); full suite also passes under the `sanitize` preset (ASan+UBSan);
both example binaries still run correctly.

### M5 — polish + hello-world
- Flesh out `examples/hello_world` into something that actually exercises
  the stack meaningfully (e.g. a coroutine that awaits a timer, prints,
  spawns a couple of abandoned background tasks, then the loop drains them
  before exiting) - well beyond today's version (M2, a single
  `promise`/`future` pair with no loop or coroutines yet), itself already
  a step up from the original M0 walking-skeleton placeholder.
- Fill in real unit test coverage for future/promise/loop/coroutine
  (M0–M4 land with tests per-component; this milestone is about
  integration-level coverage and edge cases: abandoned futures, exceptions
  crossing `co_await`, allocator propagation).

### Stretch / explicitly deferred (not part of the milestones above)
- **Bare-metal embedded port**: a second `est::platform` backend (no OS,
  interrupt-driven clock tick, and — reintroduced at that point, since M1
  removed it as premature for hosted-Linux (see M1's "Revised" note) —
  real interrupt-mask critical sections) plus whatever build-system work
  a freestanding/cross toolchain needs (a separate CMake toolchain file,
  `-ffreestanding`, no `import std;` reliance on parts of the standard
  library an embedded target can't provide). The M1 platform seam and the
  int+pointer `est::mutex` are designed now specifically so this is a new
  backend later, not a framework rewrite — but the actual port (and
  picking a target chip/board) is out of scope until the hosted build is
  solid.
- Multi-loop / multi-shard execution on hosted platforms (a second,
  distinct reason `est::mutex` would need real protection again: cross-loop
  handoff instead of interrupt/mainline handoff).
- Real I/O reactor (epoll/io_uring on hosted; interrupt-driven peripheral
  I/O on bare metal) integration into the loop.

### Issue #45: `yield_execution()` (done)

`est::yield_execution(loop&) -> future<void>` (`est/src/promise.cppm`):
one line of sugar over `sleep_for(loop_ref, loop::clock::duration::zero())`,
not a new primitive. Landing in `pending_timers_` rather than `ready_` is
the whole trick - `loop::run_impl()` always fully drains `ready_` before
ever looking at `pending_timers_`, so `co_await yield_execution(loop);`
lets whatever's already ready run first (however many rounds that takes),
then resumes the caller. Inherits `sleep_for()`'s existing abandonment/
lifetime behavior unchanged, rather than a bespoke `ready_node` needing
its own story re-derived from scratch. Two tests added
(`loop_tests.cpp`): basic readiness (not ready until `run_until_idle()`
drains it), and an ordering test proving already-ready `then()`-chained
work actually runs before a `yield_execution()`-chained continuation
queued at the same point.

**Verified in the pinned Docker devenv:** 95/95 tests pass (2 new);
`clang-format`/`clang-tidy` clean; full suite passes under the `sanitize`
preset (ASan+UBSan) too; both example binaries (`hello_world`,
`sleep_sort`) still run correctly.

### `est::intrusive_list<T>` switched from LIFO to FIFO (done)

Motivated by a design discussion on a separate, not-yet-merged branch
adding `est::yield_execution(loop&) -> future<void>` (issue #45): a
cheaper alternative to routing it through `est::loop`'s timer queue was
raised - return an already-ready future with an empty continuation, or a
bespoke node handed straight to `loop_ref.enqueue_ready()`, to re-enter
`est::loop`'s own ready-queue directly instead of going through
`pending_timers_` at all. Both ideas turned out to be broken by
`intrusive_list<T>`'s
*original* policy: LIFO (`enqueue()`/`dequeue()` both at `head_`,
documented as deliberate - "nothing built on any of those queues needs
FIFO fairness"). Re-entering `ready_` directly under LIFO means the
newly-enqueued node runs *next*, cutting ahead of whatever was already
queued - exactly backwards from "give other ready work a turn first."

Rather than work around that with a second, separately-drained list (the
other alternative raised, still cheaper than the timer queue but its own
new `loop` primitive), fixed it at the source: `intrusive_list<T>` is now
FIFO. A singly-linked list gets O(1) enqueue *and* O(1) dequeue under
FIFO the same way it did under LIFO - one extra `tail_` pointer,
`dequeue()` unchanged (still only ever touches `head_`), `enqueue()`
appends at `tail_` instead of pushing at `head_`.

This is every current user of `intrusive_list<T>` at once, not a
narrowly-scoped fix - `est::loop`'s `ready_`, `est::future_state<T>`'s
continuation waiters, and `est::mutex`'s `waiters_` all share the one
template. The mutex side effect is a genuine improvement, not just
incidental: waiters are now handed the lock first-come-first-served
instead of most-recently-queued-first, matching what most callers would
assume about a mutex without reading this file. Updated to match:
- `est/tests/intrusive_list_tests.cpp`: the LIFO order test inverted to
  FIFO; the `drain()` visit-order test's expected sequence flipped;
  added a regression test for the `tail_` pointer specifically (dequeuing
  down to empty and then enqueuing again must not leave `tail_` dangling
  from the emptied list).
- `est/tests/mutex_tests.cpp`: `"unlock() resumes queued waiters in LIFO
  order"` renamed and inverted to FIFO order.
- `est/tests/loop_tests.cpp`: `"stop() interrupts the current drain pass
  before further ready work runs"` had its two `then()` registrations
  swapped - the test relies on knowing which of two ready continuations
  the loop dequeues first, which is now determined by registration order
  instead of its reverse.
- `est/src/future.cppm`, `docs/wiki/Coroutines.md`: doc comments
  asserting LIFO order updated to FIFO.
- `yield_execution()` itself (issue #45, separate branch/PR) is
  unaffected by this either way - it goes through `pending_timers_`
  rather than `ready_` regardless of `ready_`'s own ordering policy,
  since FIFO fixes the *cheap-trick* idea's correctness but doesn't
  remove the timer-queue's own overhead (heap insert/pop,
  `pending_timers_`'s linear search+erase, a `platform::sleep_until()`
  call); that remains a separate, not-yet-taken optimization.

**Verified in the pinned Docker devenv:** 93/93 tests pass (1 new, 3
inverted in place - this branch predates issue #45's own 2 new tests,
still on a separate branch); `clang-format`/`clang-tidy` clean; full
suite passes under the `sanitize` preset (ASan+UBSan) too; both example
binaries still run correctly.

**PR #49 review follow-up:** the repo owner suggested a sentinel
(dummy) node instead of a plain `tail_` pointer that could be null - a
real `intrusive_list_node sentinel_` member that `tail_` always points
*through*, either at the last real node's own `next` slot or, when the
list is empty, at the sentinel's. `enqueue()` drops its branch entirely
(`tail_->next = &node; tail_ = &node;` works unconditionally - `tail_`
is never actually null); `dequeue()` reads the real head from
`sentinel_.next` instead of a separate `head_` field. Re-verified:
94/94 tests pass, `clang-format`/`clang-tidy` clean, `sanitize` preset
clean, both examples still run.

### `yield_execution()` optimized: direct `ready_node`, no timer queue (done)

Follow-up to issue #45, once FIFO (above) landed: the original
`sleep_for(loop_ref, 0)` sugar paid for a full timer round-trip
(`schedule_timer()`'s `timer_queue` heap insert, `fire_ready_timers()`'s
linear search+erase against `pending_timers_`, and the
`platform::sleep_until()` call `run_impl()` makes before it ever checks
`pending_timers_`) for something with no real deadline to track - it
only ever needed "run after whatever's already ready," which FIFO now
gives `ready_` directly.

Replaced with `detail::yield_resume_node` (`est/src/promise.cppm`) - a
plain `ready_node` holding a `promise<void>`, handed straight to
`loop_ref.enqueue_ready()`:

```cpp
[[nodiscard]] inline auto yield_execution(loop& loop_ref) -> future<void> {
  auto [prom, fut] = make_promise_future<void>(loop_ref);
  auto* node = loop_ref.allocator().template new_object<detail::yield_resume_node>(std::move(prom));
  loop_ref.enqueue_ready(*node);
  return std::move(fut);
}
```

Re-entering `ready_` directly *before* PR #49's FIFO switch would have
cut this node in line ahead of everything else (LIFO's answer to "does
this run before or after what's already queued" is backwards for a
yield) - exactly the bug the FIFO PR's own motivation described. Safe
now.

One thing the old `sleep_for()`-based version got for free that a fresh
`ready_node` doesn't: completing on abandonment. `sleep_for()`/
`sleep_until()`'s own `concrete_timer_node<Fn>::destroy()` just
deallocates on abandonment, silently dropping its promise - the "broken
promise, future simply never becomes ready" default every other
`est::promise<T>` in this codebase otherwise has, *without* the
exception-completing fix `mutex::lock_resume_node`/`acquire_resume_node`
needed (PR #37 review, `docs/wiki/Coroutines.md`). That's latent, real,
and still present in `sleep_for()`/`sleep_until()` today - a coroutine
`co_await`-ing either one, abandoned when its loop is destroyed first,
leaks its own frame, for the identical structural reason mutex's
resume nodes did before that fix. Out of scope to fix here (not part of
this optimization's blast radius), but `yield_resume_node` was written
*without* inheriting that bug: `destroy()` completes its promise with an
exception when `run()` never happened, matching `lock_resume_node`'s
pattern. A new regression test proves it -
`"destroying a loop with a coroutine co_await-ing yield_execution()
still pending leaks nothing"` (`loop_tests.cpp`), directly mirroring
mutex's own `"destroying a mutex with a coroutine co_await-ing
acquire() still pending leaks nothing"`.

**Verified in the pinned Docker devenv:** 97/97 tests pass (3 new -
the leak test above, plus the two pre-existing `yield_execution()`
tests carried over unchanged since observable behavior didn't change);
`clang-format`/`clang-tidy` clean; full suite passes under the
`sanitize` preset (ASan+UBSan) too - the critical check here, given
this is exactly the class of bug ASan caught for mutex's own resume
nodes earlier; both example binaries still run correctly.

### `ready_node`/`timer_node::destroy()` takes `ran` as a parameter; issue #50 fixed (done)

Follow-up observation on the pattern above: every node needing different
abandoned-vs-completed `destroy()` behavior (`mutex::lock_resume_node`/
`acquire_resume_node`, `future_resume_node<T>`, `yield_resume_node`) was
tracking that with its own private `bool ran_`/`invoked_` member, set
`true` at the top of `run()`/`invoke()` and read back inside `destroy()`.
But every call site that ever invokes `destroy()` already knows, statically,
which situation it's in - `loop::destroy_guard()` (used by `run_one()`/
`fire_ready_timers()`) always calls `run()`/`fire()` immediately before
constructing the guard, and `~loop()`/`~mutex()`/`~future_state()`'s own
drains never call `run()`/`invoke()`/`fire()` at all. There's no third
call shape. So the flag was redundant per-node state duplicating
information the caller already had - moved it into `destroy()`'s own
signature instead:

```cpp
virtual void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept = 0;
```

`destroy_guard()` passes `true` (unconditionally - both its callers just
ran/fired the node); every destructor-time drain (`~loop()`'s `ready_`/
`pending_timers_`, `~mutex()`'s `waiters_`, `~future_state()`'s `waiters_`)
passes `false`. Every concrete node with abandonment logic reads the
parameter instead of a member - `ran_`/`invoked_` and the `= true`
assignment at the top of `run()`/`invoke()` both disappear entirely from
`mutex::lock_resume_node`, `mutex::acquire_resume_node`, and
`future_resume_node<T>`. `concrete_continuation<Fn, U>` and
`flatten_forwarder<T>` (`.then()`'s own nodes) take the parameter too but
currently ignore it (`bool /*ran*/`) - whether a coroutine `co_await`-ing
a `.then()`-chained future can be stranded the same way if the *upstream*
future_state is dropped first is a real, separate question, not addressed
here (same shape as issue #50, below, but not the same fix - noted for a
future pass, not filed as its own issue yet).

**Issue #50 fixed as part of this**: `sleep_for()`/`sleep_until()`'s
`concrete_timer_node<Fn>` couldn't complete its promise on abandonment at
all - `Fn` was a type-erased closure, giving `destroy()` no way to know
it held a `promise<void>`, let alone call `set_exception()` on it, so a
coroutine `co_await`-ing a pending `sleep_for()`/`sleep_until()`,
abandoned when its loop is destroyed first, leaked its own frame.
`concrete_timer_node<Fn>` is removed entirely (nothing else instantiated
it) and replaced with `detail::sleep_resume_node` - holding the
`promise<void>` directly, same shape as `yield_resume_node`:

```cpp
class sleep_resume_node final : public timer_node {
public:
  explicit sleep_resume_node(promise<void> prom) noexcept : promise_(std::move(prom)) {}
  void fire() override { promise_.set_value(); }
  void destroy(std::pmr::polymorphic_allocator<std::byte> allocator, bool ran) noexcept override {
    if (!ran) {
      promise_.set_exception(std::make_exception_ptr(
          std::runtime_error("loop destroyed while sleep_for()/sleep_until() was pending")));
    }
    allocator.delete_object(this);
  }
private:
  promise<void> promise_;
};
```

`sleep_until()` now constructs this directly instead of wrapping a
capturing lambda in a generic node - no closure type to name, one fewer
layer of indirection. A new regression test mirrors the two already
written for mutex/`yield_execution()` -
`"destroying a loop with a coroutine co_await-ing sleep_for() still
pending leaks nothing"` (`loop_tests.cpp`).

**A second, real bug found while verifying that test** (not present
before this PR - genuinely introduced by `sleep_resume_node` completing
its promise on abandonment for the first time): `~loop()` drained
`ready_` *before* `pending_timers_`. `sleep_resume_node::destroy()`
completing its promise with an exception cascades into
`future_state<void>::complete()` draining the coroutine's own pending
continuation onto `loop_.enqueue_ready()` - the *same* loop currently
mid-destruction. Since `ready_` had already been fully drained by the
time the `pending_timers_` loop ran and triggered that cascade, the
newly-appended node was never collected - a straightforward leak (7
allocations, 3 deallocations on the first sanitize run of the new test).
`mutex::lock_resume_node`'s identical abandonment-completion pattern
never hit this, because `~mutex()` and `~loop()` are different objects:
the cascade lands on a loop that either isn't being destroyed yet, or -
when it is, and destructs right after the mutex in the same scope - gets
picked up by that *loop's own* `ready_.drain()`, which hadn't run yet at
the time of the cascade. `yield_resume_node` never hit it either, for a
different reason: it lives in `ready_` directly, and `intrusive_list<T>::
drain()`'s own `while (dequeue())` loop re-checks after every node it
destroys, so a cascade back into the very list it's draining is picked
up within the same pass. `pending_timers_`'s drain, by contrast, is a
plain `for` loop over a fixed snapshot - blind to anything appended to
a *different* container (`ready_`) partway through. Fixed by draining
`pending_timers_` first, so anything it cascades into `ready_` lands
there before `ready_.drain()` ever starts (and that loop's own re-
checking absorbs it, however many rounds deep) - documented in `~loop()`'s
own doc comment as a real ordering invariant (whichever container can
feed the other must drain after it), not an arbitrary choice.

Docs updated to match (all the places `concrete_timer_node<Fn>` or a bare
`destroy(allocator)` signature were shown or named):
`docs/wiki/Coroutines.md`, `Continuation-Node-Mechanism.md`,
`Allocation-Patterns.md`, `Architecture.md`, `Loop-And-Timers.md`.

**Verified in the pinned Docker devenv:** 98/98 tests pass (1 new - the
`sleep_for()` abandonment test, which caught the `~loop()` ordering bug
above on its first sanitize run and passes clean after the reorder; the
`ready_node`/`timer_node` signature change itself needed no further new
tests, being otherwise a pure refactor of already-tested behavior);
`clang-format`/`clang-tidy` clean; full suite passes under the `sanitize`
preset (ASan+UBSan) too; both example binaries still run correctly.

### Issue #30: `loop::current()` - user-visible types no longer require an explicit `loop&` (done)

The issue's own original suggestion was `platform::get_loop()`, a method
on `est::platform` mirroring `platform::instance()`. A first pass at this
rejected that in favor of a `thread_local loop*` scoped to `loop`'s own
constructor/destructor, reasoning that `platform::interface` is safe as a
global specifically because it's stateless policy (`docs/wiki/
Loop-And-Timers.md`'s own "why an explicit `loop&`, not a global
singleton" section) and a mutable `loop*` living there would reintroduce
exactly the state that section rules out.

Revised (repo owner's own review): that reasoning solved a problem this
codebase doesn't have while creating one it does. This codebase already
has a plain, *non*-`thread_local` global doing exactly this "which one is
current" job - `platform::detail::current_instance` (`instance()`/
`override_instance()`, `platform.cppm`) - so a second global following
the identical pattern costs nothing new. `thread_local`, on the other
hand, is something this codebase has never otherwise needed, and the
repo owner is specifically targeting a future bare-metal backend
(`docs/PLAN.md`'s own stretch goal, freestanding/no-OS) where
`thread_local` may have no well-defined support at all - reaching for it
here would have introduced a genuinely new, more fragile requirement to
solve a problem a plain global already avoids just fine.

First implemented as `est::platform::detail::current_loop_context`, a
plain `void*` global living in `:platform` alongside `current_instance`,
with plain accessor functions mirroring `instance()`/`override_instance()`
- not `loop*` (this partition sits below `:loop` in the dependency DAG, so
it can never name `est::loop` directly; `est::loop` performs the cast on
both sides, safe by construction since the only two call sites that ever
touch it are `loop`'s own constructor and destructor).

**Revised again (repo owner's own review): `get_current_loop_context()`
should be a method of the platform instance**, not a second free-standing
global living next to `current_instance`. Moved onto `platform::interface`
itself as a plain data member, with `get_current_loop_context()`/
`set_current_loop_context()` as ordinary (non-virtual) methods on it -
still not virtual, for the same reason as before: nothing about "hold a
pointer, hand it back" is backend-specific behavior the way `now()`/
`sleep_until()` genuinely are, so there's nothing here for a derived class
to override:

```cpp
// est::platform::interface
[[nodiscard]] auto get_current_loop_context() const noexcept -> void* {
  return current_loop_context_;
}
void set_current_loop_context(void* context) noexcept { current_loop_context_ = context; }
private:
  void* current_loop_context_ = nullptr;

// est:loop
[[nodiscard]] static auto current() -> loop& {
  auto* const context = platform::instance().get_current_loop_context();
  check(context != nullptr, "est::loop::current(): no loop is current (issue #30) ...");
  return *static_cast<loop*>(context);
}
```

The two designs behave identically for every case this codebase actually
exercises: `platform::instance()` only ever points at one `interface` at a
time, and every `override_instance()` guard in this codebase brackets the
full lifetime of any loop constructed under it (`loop_tests.cpp`,
`timer_tests.cpp`) - the loop's constructor and destructor always run
against the same installed instance. The one corner case the per-instance
version doesn't defend against - swapping the platform instance out from
under a still-live loop, rather than around one, which would let that
loop's destructor clear a *different* instance's (and possibly a different
loop's) slot - isn't checked for here, same as `loop::current()`'s own
nesting precondition isn't a full RAII stack; documented on `loop`'s own
top comment (`loop.cppm`) rather than guarded against, since nothing in
this codebase does this today.

**Revised a third time (repo owner's own PR review comments): `platform::
interface` should be a pure interface, full stop** - every method pure
virtual, no data members of its own. The previous revision had put
`current_loop_context_` directly on `interface`, with non-virtual
accessors - "hold a pointer, hand it back" not being backend-specific
behavior was true, but it meant `interface` still had one member and one
pair of methods a derived class *couldn't* override, on top of
`reset_loop_stall_detection()`/`detect_loop_stall()` (the long-running-
callback detector, predating issue #30) which already had that exact
shape - a default body backed by a member (`stall_start_`) declared
directly on `interface`. Per review, both pairs move onto `hosted_stdcpp`
entirely - state and logic alike - leaving `interface` with nothing but
pure virtual declarations:

```cpp
// est::platform::interface - no data members anywhere on this class
[[nodiscard]] virtual auto get_current_loop_context() const noexcept -> void* = 0;
virtual void set_current_loop_context(void* context) noexcept = 0;
virtual void reset_loop_stall_detection() noexcept = 0;
virtual void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept = 0;

// est::platform::hosted_stdcpp - owns the state, implements all four
[[nodiscard]] auto get_current_loop_context() const noexcept -> void* override {
  return current_loop_context_;
}
void set_current_loop_context(void* context) noexcept override { current_loop_context_ = context; }
void reset_loop_stall_detection() noexcept override { stall_start_ = now(); }
void detect_loop_stall(std::chrono::steady_clock::duration threshold) const noexcept override {
  const auto elapsed = now() - stall_start_;
  if (elapsed > threshold) { printdbg(...); }
}
private:
  std::chrono::steady_clock::time_point stall_start_;
  void* current_loop_context_ = nullptr;
```

Every test fake in `est/tests/` deriving from `interface` (`fake_platform`
and `jumping_platform` in `loop_tests.cpp`, `stub_platform` in
`platform_tests.cpp`, `fake_platform` in `timer_tests.cpp`) needed new
overrides of all four, since none could compile as an abstract class
otherwise. `jumping_platform` - whose entire purpose is exercising the
long-running-callback path - lost its own "relies on inheriting
`interface`'s default implementation" doc comment and gained a real
`reset_loop_stall_detection()`/`detect_loop_stall()` pair duplicating
`hosted_stdcpp`'s own logic against its own `stall_start` member; the
other three fakes (which never trigger that path) got no-op bodies
instead. `fake_platform`/`jumping_platform` in `loop_tests.cpp`, the two
that actually construct an `est::loop` under them, needed a real, working
`get_current_loop_context()`/`set_current_loop_context()` pair backed by
their own member too - a no-op there would have silently broken
`loop::current()` for every test using them; `stub_platform` and
`timer_tests.cpp`'s `fake_platform` never construct a loop, so a
not-exercised-but-still-correct member (`stub_platform`) or a bare
`nullptr`/no-op pair (`timer_tests.cpp`, which drives `est::timer_queue`
directly and never `est::loop` at all) were both fine there.

A single slot with a checked precondition against nesting (constructing
a second loop while one is already current is an `est::check()`
failure), not a push/pop RAII stack like `platform::override_instance()`'s
- simpler to reason about, and nothing in this codebase has a legitimate
reason to nest loops today.

**Revised a fourth time (repo owner's own PR review): "creating a loop
and setting it as current should be the caller's responsibility," not
something `loop`'s own constructor decides unconditionally.** The
auto-registration above - stores `this` in the constructor, clears it in
the destructor - got reverted outright: `est::loop`'s constructor and
destructor are back to knowing nothing about "current" at all. In its
place, a new method, `make_current()`, does the registration explicitly
and returns an `est::scope_exit`-based guard, modeled directly on
`platform::override_instance()`'s own RAII shape:

```cpp
[[nodiscard]] auto make_current() noexcept {
  check(!detail::loop_is_current,
        "est::loop::make_current(): another loop is already current (issue #30) - only one "
        "loop can be current at a time");
  detail::loop_is_current = true;
  platform::instance().set_current_loop_context(this);
  return scope_exit([]() noexcept {
    detail::loop_is_current = false;
    platform::instance().set_current_loop_context(nullptr);
  });
}
```

The nesting-precondition check moved with it, but *not* onto
`get_current_loop_context() == nullptr` the way the constructor's own
check used to read it - that question changes meaning under the fifth
revision below (it becomes "is nothing explicitly registered," not
"nothing is registered at all," once a fallback exists). A dedicated flag,
`detail::loop_is_current` (a plain, non-`thread_local` `bool` local to
`:loop`, same bare-metal reasoning as this section's every other global),
tracks "has some loop called `make_current()`" independently of whatever
`:platform`'s own storage returns.

**Revised a fifth time, in the same round of review, to actually deliver
the ergonomic goal issue #30 was chasing: a caller that never wants to
think about `est::loop` at all.** Without this, `loop::current()` would
simply fail its own precondition until *something* called
`make_current()` - workable, but no more convenient than before for the
simplest case (a script, an example) that has no loop to register in the
first place. Per review: **`hosted_stdcpp` should create a loop and
return it from `get_current_loop_context()`** - lazily, on first use,
whenever nothing has been explicitly registered:

```cpp
// est::platform::hosted_stdcpp (now in its own module - see below)
[[nodiscard]] auto get_current_loop_context() const noexcept -> void* override {
  if (explicit_loop_ != nullptr) {
    return explicit_loop_;
  }
  if (!default_loop_.has_value()) {
    default_loop_.emplace();
  }
  return &*default_loop_;
}
void set_current_loop_context(void* context) noexcept override { explicit_loop_ = context; }
private:
  void* explicit_loop_ = nullptr;
  mutable std::optional<loop> default_loop_;
```

`explicit_loop_` (whatever a caller most recently registered via
`make_current()`) takes priority whenever set; `default_loop_` is
`hosted_stdcpp`'s own fallback, constructed once and kept for as long as
that `hosted_stdcpp` instance lives - not torn down and rebuilt every time
an explicit registration comes and goes. A caller using the fallback
drives it exactly like any other loop: `est::loop::current().run_until_idle();`,
without ever writing `est::loop loop;` themselves.

This is what actually forced the module split proposed in review:
`hosted_stdcpp` now has to name `est::loop` to construct its own
fallback, and `:platform` sits below `:loop` in the module dependency DAG
specifically so it never has to - naming `est::loop` there would invert
it. `hosted_stdcpp` moved out of `platform.cppm` entirely, into its own
module, `est/src/platform/hosted_stdcpp.cppm` (`:platform.hosted_stdcpp`)
- a *different* partition is free to import `:loop` without inverting
anything, since only `:platform` itself (the abstraction every backend
implements) is barred from it. `:platform` itself shrank down to exactly
what its own class comment now says: `interface` (pure virtual, no
concrete backend), `instance()`/`override_instance()`, `printdbg()` - and
`detail::current_instance` now starts `nullptr`, since `:platform` can no
longer construct a `hosted_stdcpp` to seed it with. `est/src/est.cppm`
(the umbrella every `import est;` consumer actually triggers) is where
the real one gets installed instead - a process-lifetime
`hosted_stdcpp`, installed as the permanent default via the exact same
`override_instance()` every test already uses to install a *temporary*
one, just never letting the returned guard go out of scope:

```cpp
// est.cppm
namespace est::detail {
inline platform::hosted_stdcpp default_platform_instance{};
inline auto default_platform_guard = platform::override_instance(default_platform_instance);
} // namespace est::detail
```

Notably, this is the same split **issue #6** once proposed ("split
`platform.cppm` into a declare-only interface + a link-time
implementation") and this codebase closed as not planned, reasoning
"no concrete payoff with only one backend exists" - true at the time, but
issue #30's own fallback-loop requirement is exactly the concrete payoff
that was missing then. Revisiting a past "closed as not planned" decision
once its stated reasoning stops holding is the same stance this codebase
already took on issue #11 (reopened once a real `shared_from_this()` call
site appeared) - not a reversal of the earlier judgment, just new
information the earlier judgment explicitly said would change it
("revisit once/if a second backend... is actually being built" - the
`:platform.hosted_stdcpp`/`:platform` split isn't a second *backend*, but
it's the same underlying module-boundary tradeoff issue #6 was actually
asking about).

A genuinely new test exercises this fallback specifically:
`est/tests/loop_tests.cpp`'s "`loop::current()` with nothing explicitly
registered falls back to hosted_stdcpp's own default loop" - the one test
in that file with no `make_current()` call and no `override_instance()`
either, running against the real, default `platform::instance()` on
purpose. `yield_execution()`, not `sleep_for()`/`sleep_until()`: this
exercises the real backend, so a timer-based wait would be a genuine (if
short) wall-clock sleep. Every other issue #30 test that used to rely on
constructor auto-registration now calls `loop.make_current()` explicitly
right after constructing its loop - `loop_tests.cpp`, `future_tests.cpp`'s
three coroutine tests, and `mutex_tests.cpp`'s no-argument-constructor
test.

**Revised a sixth time, two smaller follow-ups from the same round of
review.**

First: **why not forward-declare `loop` in `:platform`, instead of an
opaque `void*`?** Tried, and it works: `get_current_loop_context()`/
`set_current_loop_context()` now return/take a genuinely typed
`est::loop*`.

```cpp
// est::platform (platform.cppm) - no `import :loop;` anywhere in this file
export namespace est { class loop; }
```

The forward declaration has to be `export`ed. Confirmed directly against
this toolchain (clang, the pinned devenv): a plain, non-exported `class
loop;` gets diagnosed as redeclaring an entity with module-private
linkage the moment `:loop`'s own `export class loop { ... };` tries to
attach to it -

```
error: cannot export redeclaration 'loop' here since the previous declaration has module linkage
```

- because `:platform` and `:loop` are different partitions of the *same*
module, and a non-exported declaration in one partition has linkage
private to that partition alone. The `export`ed version compiles cleanly:
an exported declaration in one partition genuinely attaches to the real
definition given in another partition of the same module, without either
partition needing to `import` the other. `:platform` still can't
construct, dereference, or otherwise complete `est::loop` - naming the
type is all this needed, and is as far as `:platform` itself ever goes;
completing it is still exclusively `hosted_stdcpp`'s job (`import
:loop;`, in its own, separate partition). `loop::current()`'s own
`static_cast<loop*>(...)` and its accompanying NOLINT are both gone -
`get_current_loop_context()` already returns the right type.

Second: **`import est;` installing `hosted_stdcpp` as a process-lifetime
side effect (shown in the `est.cppm` snippet above) is the library making
a decision - which backend, if any - that isn't the library's to make
silently.** Reverted: `est.cppm` no longer installs anything, and goes
back to being a plain umbrella of `export import`s. Each consumer now
does its own installation, at the top of its own entry point:

```cpp
// examples/hello_world/main.cpp, examples/sleep_sort/main.cpp
auto main() -> int {
  est::platform::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);
  // ...
}
```

`est/tests/`'s own Catch2 binary needed the equivalent for its one shared
process: previously linked against `Catch2::Catch2WithMain` (Catch2's own
pre-built `main()`), it now links `Catch2::Catch2` instead and supplies a
small `est/tests/test_main.cpp` of its own - construct `hosted_stdcpp`,
`override_instance()` it, then `Catch::Session().run(argc, argv)` - so
the installation happens exactly once, before any `TEST_CASE` runs,
rather than being every individual test file's own concern. Every test
that needs a *different* backend still uses `override_instance()` to
retarget it for its own scope, unchanged - this only supplies the one
installed underneath that, so `platform::instance()` is never null to
begin with.

No new tests came out of this revision (all pre-existing behavior, same
106 tests as before) - the entire point was that plumbing which backend
gets installed, and where, shouldn't change any test's own outcome.

**Revised a seventh time: `hosted_stdcpp` shouldn't be one of `est`'s own
partitions at all - not even re-exported through `est.cppm`.** The sixth
revision's own module split (`:platform.hosted_stdcpp`) genuinely solved
the `:platform`-can't-import-`:loop` problem, but it still left
`hosted_stdcpp` inside `est`'s own module boundary: `est.cppm` had to
`export import :platform.hosted_stdcpp;` for `examples/*/main.cpp` and
`est/tests/test_main.cpp` to name `est::platform::hosted_stdcpp` at all
(a partition is only visible outside its module if the primary module
interface unit re-exports it), which meant `hosted_stdcpp` was always
compiled as part of `est` and logically part of its exported surface,
whether or not a given consumer's program ever touched it.

Per review: move it to `estext`, a wholly separate module with its own
CMake target (`estext/CMakeLists.txt`, `add_library(estext STATIC)`,
linked `PUBLIC` against `est::est`), not a partition of `est` at all:

```cpp
// estext/src/hosted_stdcpp.cppm
export module estext;
import est;
import std;

export namespace estext {
class hosted_stdcpp final : public est::platform::interface {
  // ... identical to the :platform.hosted_stdcpp version, just
  // est::loop/est::platform::printdbg/est::platform::interface qualified
  // explicitly now, since this is namespace estext, not namespace
  // est::platform anymore.
};
}
```

`import est;` alone now gives a consumer the complete framework with
*zero* trace of any concrete backend; a consumer that wants a working one
opts in with a second, separate import - `import est; import estext;` -
exactly mirroring how a future bare-metal backend would be its own
similarly separate module, never touching `estext`. Note what `estext`
does *not* need that `:platform` itself does: since it's not a partition
bound by `est`'s own internal module DAG, `import est;` alone already
hands it the complete, already-defined `est::loop` - no forward
declaration trick required (that trick stays exactly where it was, inside
`:platform` itself, purely for `platform::interface`'s own pure virtual
method signature).

This did surface one real CMake ordering wrinkle, not a design problem so
much as a consequence of `est/tests/` being a *nested*
`add_subdirectory()` call inside `est/CMakeLists.txt` itself:

```
root CMakeLists.txt
  add_subdirectory(est)       # defines est::est; also processes
                               # est/tests/ internally (its own
                               # add_subdirectory(tests) call) - but
                               # estext::estext doesn't exist yet at
                               # that point
  add_subdirectory(estext)    # needs est::est, so can't come first either
```

`est/tests/CMakeLists.txt` needs `estext::estext` (`test_main.cpp`'s own
`import estext;`), but by the time it's processed, `add_subdirectory(estext)`
hasn't run yet - and `estext` itself can't move earlier, since it needs
`est::est` to already exist. Resolved without restructuring `est/CMakeLists
.txt`'s existing, self-contained `EST_BUILD_TESTS` handling: the one extra
link edge is added from the root `CMakeLists.txt` instead, once both
targets exist (`CMP0079`, default `NEW` under this project's
`cmake_minimum_required(VERSION 3.28)`, lets a target defined in one
directory be given more link dependencies from a different directory's
scope):

```cmake
# root CMakeLists.txt
add_subdirectory(est)
add_subdirectory(estext)

if(EST_BUILD_TESTS)
  target_link_libraries(est_tests PRIVATE estext::estext)
endif()
```

`examples/hello_world/CMakeLists.txt`/`examples/sleep_sort/CMakeLists.txt`
don't hit this - `add_subdirectory(examples/...)` already runs after both
`est` and `estext` in the root file, so they link `estext::estext`
directly, same as `est::est`.

Verified in the pinned Docker devenv: reconfigured from a clean build
directory (to catch any ordering issue the wrinkle above could have
introduced, not just an incremental rebuild) - 106/106 tests still pass,
`clang-format`/`clang-tidy` clean across every touched and new file
(`estext/src/hosted_stdcpp.cppm`, `est/src/est.cppm`, both examples'
`main.cpp`, `est/tests/test_main.cpp`), full suite passes under the
`sanitize` preset (also reconfigured from clean) too, both example
binaries still run correctly.

**Revised an eighth time, the last: `loop.cppm` shouldn't be touched by
this issue at all.** Per review: "the code to set it as the current
belongs maybe to a free function somewhere else, maybe in the util."
Every revision from the fourth onward had put this mechanism directly on
`est::loop` itself - `make_current()` (an instance method) and `current()`
(a static method) - meaning the class's own diff carried this entire
issue, even though "a primitive ready-queue-and-timers type" and "an
opt-in convenience for not threading a `loop&` by hand" are unrelated
concerns. Moved both into a new partition, `est/src/util/current_loop.cppm`
(`:util.current_loop`), as free functions instead of methods:

```cpp
// est:util.current_loop
[[nodiscard]] auto make_current_loop(loop& loop_ref) noexcept {
  check(!detail::loop_is_current, "est::make_current_loop(): ...");
  detail::loop_is_current = true;
  platform::instance().set_current_loop_context(&loop_ref);
  return scope_exit([]() noexcept {
    detail::loop_is_current = false;
    platform::instance().set_current_loop_context(nullptr);
  });
}

[[nodiscard]] auto current_loop() -> loop& {
  auto* const context = platform::instance().get_current_loop_context();
  check(context != nullptr, "est::current_loop(): ...");
  return *context;
}
```

`detail::loop_is_current` (the nesting flag) moved into this same new
file, still non-exported and scoped to just this one partition.
`est/src/loop.cppm` itself is now byte-for-byte identical to `main` -
confirmed directly (`git diff origin/main -- est/src/loop.cppm` produces
no output) rather than just eyeballed. This partition needed no forward-
declaration trick the way `:platform` did (previous revision): it's an
ordinary partition of `est`, free to `import :loop` and `:platform`
directly, sitting above both in the dependency DAG - only `:platform`
itself (and anything that must stay *below* `:loop`) is barred from
importing it.

Every call site using the old member-method names updated to the free
functions: `promise.cppm` (`make_promise_future()`'s no-arg overload,
`sleep_until()`/`sleep_for()`/`yield_execution()`'s no-arg overloads),
`future.cppm` (`promise_type`'s two delegating constructors and two
`operator new` overloads), `sync/mutex.cppm` (`mutex()`'s no-arg
constructor), and every test that used to call `loop.make_current()`/
`est::loop::current()` directly (`loop_tests.cpp`, `future_tests.cpp`,
`mutex_tests.cpp`). Each of these files already needed `import
:util.current_loop;` added (or, for `mutex.cppm`, added even though it
already transitively depended on `:promise` - a private `import` in one
partition doesn't propagate visibility to that partition's own
importers, so `current_loop()` needed its own direct import there too).

No test behavior changed - this was a pure rename plus a relocation, not
a design change. Verified in the pinned Docker devenv: reconfigured `ci`
from scratch (nothing had reason to affect configure-time behavior, but
checked anyway) - 106/106 tests still pass, `clang-format`/`clang-tidy`
clean across every touched file (one `clang-format -i` needed afterward,
for a `TEST_CASE` title string that grew past the column limit when
renamed), full suite passes under `sanitize` too, both examples still run
correctly.

Every loop-taking free function gained a matching overload built on
`current_loop()`: `make_promise_future<T>()`, `sleep_for()`/
`sleep_until()`, `yield_execution()` (all `est:promise`). `est::mutex`
gained a matching no-argument constructor.

**A coroutine returning `est::future<T>` can drop the `est::loop&`
parameter too** - the issue's own "at least for user-visible types"
framing extended to cover this, not just the free-function helpers.
`promise_type` gained two more constructor/`operator new` pairs beyond
the original loop-taking one:

```cpp
template <class First, class... Rest>
  requires(!std::same_as<std::remove_cvref_t<First>, loop>)
explicit promise_type(First& /*unused*/, Rest&... /*unused*/) : promise_type(loop::current()) {}

promise_type() : promise_type(loop::current()) {}
```

(Shown here in the delegating form a later review pass produced - see
below.) The `requires` clause is load-bearing: without it, this
constructor and the original `promise_type(loop&, Args&...)` would be
genuinely ambiguous for a call that *does* pass a loop first - a bare
parameter pack happily absorbs a leading `loop&` into its own pack, so
both candidates deduce to the identical actual parameter list for such a
call, a tie conversion ranking alone can't break (confirmed by reasoning
through the standard's partial-ordering rules before writing this, not
just by testing - the constrained-SFINAE approach was chosen specifically
to not have to *rely* on partial ordering, since it's a genuinely easy
corner of overload resolution to get subtly wrong). A coroutine with no
parameters at all needs its own non-template overload, since the
constrained one still requires at least one `First`. `operator new`
gained the identical three-way split, matched against the same argument
list by the same "promise constructor arguments" rule, so whichever
constructor is chosen and whichever `operator new` is chosen always agree
on which loop to use.

**Revised (repo owner's own PR review): all three constructors should
delegate to a single one that actually builds `state_`.** The original
version above independently repeated the same
`detail::future_promise_result<T>(shared_ptr<future_state<T>>::make(...))`
initializer three times. Since the first (loop-taking) constructor is
already a template accepting an empty `Args...` pack, it already covers
"just a `loop&`, nothing else" - so the other two now delegate straight to
it: `promise_type(First&, Rest&...) : promise_type(loop::current()) {}`
and `promise_type() : promise_type(loop::current()) {}`. Overload
resolution among the class's own constructors still correctly picks the
`(loop&, Args&...)` template for that single-argument delegating call (the
`requires`-constrained one is excluded by its own clause, since the
argument's type genuinely is `loop`), so this is a pure deduplication, not
a behavior change. `operator new`'s three-way split wasn't touched -
review flagged only the constructors, and each `operator new` overload
was already a single-line call straight to
`detail::coroutine_frame_alloc()`, with nothing left to deduplicate.

`est::future<T>::get_promise()`, the issue's other original suggestion
(let a bare `future<T>` be constructed and a matching `promise<T>`
extracted from it later), wasn't implemented - it doesn't add anything
`make_promise_future<T>()`'s own no-argument overload doesn't already
cover for the same underlying goal ("create a promise/future pair
without an explicit loop"), and would be a second, redundant way to
reach it.

Tests added: `loop::current()`'s happy path (returns the loop actually
constructed); each new no-`loop&` overload
(`make_promise_future<T>()`, `sleep_for()`/`sleep_until()`/
`yield_execution()`, `mutex()`); three coroutine shapes (some parameters
but no `loop&`, no parameters at all, and one that genuinely suspends and
resumes through `loop::current()` rather than just running synchronously
to completion, to prove the frame was actually allocated against the
right loop). `est::check()`'s own failure path (no loop current, or a
second loop constructed while one already is) isn't unit-tested, same
stance as every other checked precondition in this codebase
(`est/tests/check_tests.cpp`'s own doc comment).

One more test came out of the fourth/fifth revisions above: `hosted_stdcpp`'s
own lazy-fallback loop (`loop_tests.cpp`'s "`loop::current()` with nothing
explicitly registered falls back to hosted_stdcpp's own default loop" -
this section's own fuller description of it) - bringing the total to 8
new tests, not 7.

Docs updated: `docs/wiki/Coroutines.md`'s calling-convention section
(also fixed a small pre-existing staleness there - the shown
`promise_type` code snippet still had a `loop_` member removed in an
earlier PR #37 follow-up), `docs/wiki/Loop-And-Timers.md`'s own new
`current_loop()` section (rewritten again across the fourth through
eighth revisions), `docs/wiki/Home.md`'s quick overview, `docs/wiki/
Architecture.md`'s module-DAG diagram (added `:util.current_loop`, its
own short section, and its new `estext` section - replacing the sixth
revision's `:platform.hosted_stdcpp` partition/edge with the seventh's
wholly separate `estext` module, plus the `est::loop*`-not-`void*` and
no-auto-install notes carried over from the sixth).

**Verified in the pinned Docker devenv, after the eighth revision
above:** reconfigured from a clean build directory for both the `ci` and
`sanitize` presets (not just an incremental rebuild) - 106/106 tests pass
(8 new, unchanged since the fifth revision); `clang-format`/`clang-tidy`
clean across every touched and new file (`est/src/util/current_loop.cppm`,
`estext/src/hosted_stdcpp.cppm`, `est/src/est.cppm`, `est/src/promise.cppm`,
`est/src/future.cppm`, `est/src/sync/mutex.cppm`, both examples'
`main.cpp`, `est/tests/test_main.cpp`); `est/src/loop.cppm` confirmed
byte-identical to `main`; full suite passes under the `sanitize` preset
(ASan+UBSan) too; both example binaries still run correctly.

### Issue #63: `continuation_node<T>`/`concrete_continuation<Fn, U>` revisited (done)

The issue's own question: `Fn` alone already determines both `T` (the
callback's parameter) and `U` (its flattened result), so does
`continuation_node<T>`'s split - a shared `run()` at the `T`-only layer,
forwarding to a separately virtual `invoke(future_state<T>&)` each
`concrete_continuation<Fn, U>` implements - still pull its weight? The
issue floated two directions if not: `bind_owner()`/`owner_` as a mixin,
or dropping the split entirely and duplicating that plumbing by hand "for
less codegen." It also explicitly allowed "don't" as a valid outcome.

Looking at what `run()` actually did settled it: `void run() final {
invoke(*owner_); }` - and `invoke()`'s only ever caller was that one line.
The `future_state<T>& state` parameter every `invoke()` override took was
therefore always just `*owner_`, read back through a parameter instead of
read directly. So the "one shared implementation instead of once per
`(Fn, U)`" the type hierarchy's own doc comment justified this split
with was sharing a single pointer-dereference-and-call - real, but not
worth what it costs: `est::loop`'s ready-queue holds nothing more specific
than a `ready_node&` (`ready_.dequeue()`, `run_one()`), so `run()`'s own
dispatch there can never statically know which concrete type it's about
to make a *second*, separately virtual `invoke()` call into - not
something ThinLTO whole-program devirtualization (`docs/wiki/
Global-Lookup-Codegen.md`'s own methodology, used to check exactly this
kind of claim in the current-loop-only-experiment work) can fix either,
since multiple different concrete node types genuinely coexist in the
same ready-queue at once. Every continuation this framework ever runs -
every `.then()`, every flattening forward, every coroutine resumption -
was paying for two indirect calls to get to one line of actual work.

Resolved by dropping the split, not turning it into a mixin: `bind_owner()`/
`owner_` stay on `continuation_node<T>`, `protected` now instead of
`private` so each concrete node can read `owner_` directly, but
`continuation_node<T>` no longer implements `run()` or declares `invoke()`
at all. `concrete_continuation<Fn, U>` (nested in `future_state<T>`),
`flatten_forwarder<T>`, and `future_resume_node<T>` each now implement
`ready_node::run()` themselves - `run()` `final`, reading `*owner_`
directly, everything else about each of their bodies unchanged. A
distinct mixin base for `bind_owner()`/`owner_` alone (the issue's other
suggested direction) would have been strictly more ceremony for the same
result: sharing that plumbing was already free before this change (a
plain data member and a non-virtual setter, never a second vtable slot),
so `continuation_node<T>` trimmed down to just that is already the
mixin - no separate class needed. The actual saving is the removed
`invoke()` virtual call itself, on every single continuation run, for the
cost of a few bytes of duplicated `*owner_` dereference logic across
three call sites instead of one - not something worth measuring further
with `examples/probe`/LTO disassembly the way the `current_loop()`
lookup cost was: unlike that case, there's no ambiguity here for a
disassembly to resolve - two indirect calls through two separate vtable
slots is strictly more than one, on every path, by construction.

No behavior changed: every `run()` body is byte-for-byte what its
`invoke()` predecessor was, minus the now-redundant `future_state<T>&`
parameter (replaced by `*this->owner_`, read from the same member the
removed parameter was always aliasing). Docs updated: `docs/wiki/
Continuation-Node-Mechanism.md` (the type hierarchy diagram and prose,
the node-lifecycle sequence diagram, the "two calling conventions" and
"flattening" sections' code excerpts), `docs/wiki/Coroutines.md`
(`future_resume_node<T>`'s two shown snippets and its sequence diagram),
`docs/wiki/Architecture.md` and `docs/wiki/Loop-And-Timers.md` (passing
mentions of `invoke()`).

**Verified in the pinned Docker devenv:** full test suite passes,
unchanged in count (this is a pure internal refactor - no test observes
`continuation_node<T>`/`concrete_continuation<Fn, U>` directly, only
`future<T>`'s own public surface); `clang-format`/`clang-tidy` clean;
full suite passes under the `sanitize` preset (ASan+UBSan) too;
`diff-cover` coverage gate passes against `main`.

### `ready_node`/`timer_node::destroy()` replaced with a plain virtual destructor + `abandon()` (done)

Follow-up on "`ready_node`/`timer_node::destroy()` takes `ran` as a
parameter; issue #50 fixed" above: `destroy(allocator, ran)` still had to
exist as a hand-rolled virtual method purely so each concrete node could
deallocate itself through its own most-derived type - `ready_node`/
`timer_node`'s own doc comments called this out directly ("the whole
reason `destroy()` is virtual instead of the caller deallocating through
a `detail::ready_node&`"). Plain C++ already has a mechanism for exactly
this: a class with a virtual destructor deallocates through the *dynamic*
type's own visible `operator delete`, with the dynamic type's own correct
size - never the static (base) one - when deleted through a base
pointer/reference. `est::future<T>::promise_type` was already relying on
this same idea for its own coroutine frame (`coroutine_frame_alloc()`/
`coroutine_frame_dealloc()`, resolving `est::current_allocator()` fresh);
this generalizes it to every `ready_node`/`timer_node` in the codebase.

`ready_node`/`timer_node` themselves can't define a *shared* `operator
new`/`operator delete` doing this, though - that would need
`est::current_allocator()` (`est:util.current_loop`), which itself
imports `:loop`, so `:loop` importing it back would be circular (the same
constraint this file's own top comment on `:loop`/`:future` already
documents, one level further down the DAG). So every concrete node type
- `mutex::lock_resume_node`/`acquire_resume_node`, `est:promise`'s
`sleep_resume_node`/`yield_resume_node`, `est:sync.event`'s
`event_resume_node`, `est:future`'s `future_resume_node<T>`/
`concrete_continuation<Fn, U>`/`flatten_forwarder<T>` - now defines its
own pair instead, each one the same six lines:

```cpp
static auto operator new(std::size_t size) -> void* {
  return current_allocator().resource()->allocate(size, alignof(lock_resume_node));
}
static void operator delete(void* ptr, std::size_t size) noexcept {
  current_allocator().resource()->deallocate(ptr, size, alignof(lock_resume_node));
}
```

`destroy()`'s other job - completing a held `promise<T>` with an
exception when a node is deleted without ever having `run()`/`fire()`d -
splits out into a new `virtual void abandon() noexcept {}`
on `ready_node`/`timer_node`, defaulted to a no-op. Every call site that
used to pass `ran` now either calls `abandon()` first (a drain that never
ran the node - `~future_state()`, `~mutex()`, `~counting_event()`,
`loop::drain_pending()`) or skips straight to `delete` (`loop::
destroy_guard()`, used by `run_one()`/`fire_ready_timers()` right after
`run()`/`fire()` - which never counts as abandoned). This removes the
`bool ran` parameter entirely - a node with abandonment logic
(`lock_resume_node`/`acquire_resume_node`, `sleep_resume_node`/
`yield_resume_node`, `event_resume_node`, `future_resume_node<T>`)
overrides `abandon()` with just its exception-completing (or, for
`future_resume_node<T>`, frame-freeing) logic and nothing else; a node
without one (`concrete_continuation<Fn, U>`, `flatten_forwarder<T>`)
simply doesn't override it, rather than taking the parameter and
ignoring it.

`future_state<T>::allocator()` (a thin `current_allocator()` wrapper) is
now dead code - its only two callers (`then()`'s node allocation,
`fulfill()`'s flattening node allocation) both switched to a plain `new`,
which resolves the allocator internally - so it's removed rather than
left unused.

No behavior changes as a result: allocation and deallocation still
resolve `est::current_allocator()` fresh at the same points as before
(now inside each concrete node's own `operator new`/`operator delete`
rather than pre-resolved by the caller and threaded through an explicit
parameter), and `abandon()` runs at exactly the same points `destroy(...,
false)` used to. `loop::drain_pending()`'s own doc comment on why it must
run *before* `make_current_loop()`'s guard clears the current-loop slot
now covers node deletion itself, not just the coroutine frame a node's
`abandon()` might go on to free - both resolve `current_allocator()`
fresh, so both need the same ordering guarantee.

Docs updated everywhere `destroy(allocator, ran)`/`bool ran` was shown or
named: `docs/wiki/Coroutines.md`, `Continuation-Node-Mechanism.md`,
`Allocation-Patterns.md`, `Architecture.md`, `Loop-And-Timers.md`.

**Verified in the pinned Docker devenv:** existing test suite passes
unchanged (a pure refactor of already-tested abandonment/allocation
behavior, no new observable behavior to add a test for);
`clang-format`/`clang-tidy` clean across every touched file; full suite
passes under the `sanitize` preset (ASan+UBSan) too, confirming every
concrete node's `operator new`/`operator delete` pairs allocate and
deallocate through matching sizes; both example binaries still run
correctly.

### `current_allocator_new_delete<T>` mixin: the 8 hand-rolled operator new/delete pairs above deduplicated (done)

Code-review follow-up on the entry above: every one of the 8 concrete
node types that gained an `operator new`/`operator delete` pair there
(`mutex::lock_resume_node`/`acquire_resume_node`, `est:promise`'s
`sleep_resume_node`/`yield_resume_node`, `est:sync.event`'s
`event_resume_node`, `est:future`'s `future_resume_node<T>`/
`concrete_continuation<Fn, U>`/`flatten_forwarder<T>`) hand-rolled the
identical six lines, differing only in which class name got substituted
into `alignof(...)`. Factored into one CRTP mixin instead:

```cpp
template <class Derived> class current_allocator_new_delete {
public:
  static auto operator new(std::size_t size) -> void* {
    return current_allocator().resource()->allocate(size, alignof(Derived));
  }
  static void operator delete(void* ptr, std::size_t size) noexcept {
    current_allocator().resource()->deallocate(ptr, size, alignof(Derived));
  }
};
```

Added to `est:util.current_loop` (`est/src/util/current_loop.cppm`), not
a new partition of its own: it needs nothing beyond `current_allocator()`,
already defined right there, and every one of the four partitions with a
node class to update (`:future`, `:promise`, `:sync.mutex`, `:sync.event`)
already imports `:util.current_loop`. `est::detail::ready_node`/
`timer_node` (`:loop`) still can't inherit it themselves, for the
identical circular-dependency reason their own doc comments already give
for not defining a shared default in the first place: the mixin needs
`current_allocator()`, and `:util.current_loop` imports `:loop`, so
`:loop` importing it back would be circular. Each of the 8 classes now
just adds `public current_allocator_new_delete<TheClassItself>` to its
base-class list (`detail::current_allocator_new_delete<T>` from the two
call sites - `mutex::lock_resume_node`/`acquire_resume_node` and
`future_state<T>::concrete_continuation<Fn, U>` - not already inside
`namespace est::detail` themselves) instead of defining the pair by hand;
no behavior change, since the inherited pair does exactly what the
hand-rolled one did.

Docs updated: `docs/wiki/Allocation-Patterns.md` and
`Continuation-Node-Mechanism.md` (both described the pair as "every
concrete node type defines its own"), plus two stale doc-comment
cross-references to a since-removed `future_state<T>::allocator()`
method caught by the same review pass (`est/src/future.cppm`).

**`current_allocator_new_delete<T>`'s own special members, settled by
`clang-tidy`:** the mixin has no data, so its first cut left every
special member implicit (rule of zero) apart from a private default
constructor (+ `friend Derived`) guarding against unrelated construction
- but `clang-tidy` flagged that as `cppcoreguidelines-special-member-
functions` ("declares one, not the others") the moment a destructor was
added explicitly to quiet a *different* check
(`performance-trivially-destructible`, which wants an explicit
`=default` destructor even though the fully-implicit one is already
trivial). The two checks want contradictory things for this exact shape
(private constructor + `friend Derived`) - genuinely, not just a matter
of ordering: adding the destructor `performance-trivially-destructible`
asks for immediately re-triggers `cppcoreguidelines-special-member-
functions`, and removing it to satisfy that one re-triggers the first
again. Settled by keeping rule-of-zero (the actually-correct state) and
suppressing the false positive with a `NOLINTNEXTLINE` directly above
the class - which itself needs to be the literal line before the class
declaration, not several explanatory-comment lines above it:
`NOLINTNEXTLINE` only suppresses the one line immediately following the
comment, a mistake this branch's own history made once already before
being fixed.

**Verified in the pinned Docker devenv:** existing test suite passes
unchanged (pure refactor, no new observable behavior);
`clang-format`/`clang-tidy` clean; full suite passes under the
`sanitize` preset (ASan+UBSan) too.

### `abandon()`-then-`delete` deduplicated into `abandon_ready_node()`/`abandon_timer_node()` (done)

Second code-review follow-up: every drain-without-running call site
introduced by the `destroy()` → `abandon()` refactor above
(`future_state<T>::~future_state()`, `mutex::~mutex()`,
`counting_event<Mode>::~counting_event()`, and both containers
`loop::drain_pending()` itself drains) repeated the identical two-line
body - `node.abandon(); delete &node;` (or `delete entry.node;` for a
`timer_node*`) - either inline or as a one-off lambda passed to
`intrusive_list<ready_node>::drain()`. Lifted into two free functions
living in `:loop` (`est/src/loop.cppm`), right next to `ready_node`/
`timer_node`'s own definitions - the natural home, since they're the
classes that define the `abandon()` contract in the first place:

```cpp
inline void abandon_ready_node(ready_node& node) noexcept {
  node.abandon();
  delete &node;
}
inline void abandon_timer_node(timer_node& node) noexcept {
  node.abandon();
  delete &node;
}
```

Two separate, non-overloaded functions rather than one function
overloaded on `ready_node&`/`timer_node&`: every `waiters_.drain(...)`
call site passes the function directly (`waiters_.drain(detail::
abandon_ready_node);`) rather than wrapping it in a lambda, and
`intrusive_list<T>::drain(Fn fn)` deduces its own `Fn` template
parameter from that argument - deduction that only works when the name
resolves to exactly one type. An overloaded name has no single type
until *after* overload resolution has already run, so passing an
overloaded name straight to a deducing `Fn` parameter is ill-formed, not
a matter of which overload a reader would expect to be picked. Since
`timer_node`'s own abandonment never goes through `intrusive_list<T>`
(`loop::drain_pending()`'s `pending_timers_` is a plain
`std::pmr::vector`, not an intrusive list), only `abandon_ready_node` is
ever passed this way in practice - `abandon_timer_node` is always called
directly - but keeping both non-overloaded avoids the trap regardless.

Considered and rejected: lifting this into `est::intrusive_list<T>`
itself (a `drain_abandoning()` method, say). `:util.intrusive_list` is
deliberately a generic utility with no notion of `abandon()` or
allocator-routed deletion - coupling it to `ready_node`'s specific
lifecycle contract would be the wrong layer for it, even though every
current instantiation happens to be `intrusive_list<detail::ready_node>`
in practice.

No behavior change: each call site now reads `waiters_.drain(detail::
abandon_ready_node);` (or a direct call to one of the two functions)
instead of a hand-written lambda/pair of statements, but every code path
still does exactly what it did before.

**Verified in the pinned Docker devenv:** existing test suite passes
unchanged (pure refactor); `clang-format`/`clang-tidy` clean; full suite
passes under the `sanitize` preset (ASan+UBSan) too.
### Issue #66 & #67: `est::mutex` rebuilt on top of `est::counting_event` (done)

Two issues taken together, since #67 ("build mutex on top of event -
should simplify stuff") is what made #66 ("remove `mutex::acquire`, make
`mutex::lock` return `lock_guard` - make unlock visible only to lock
guard") easy to actually do: once `mutex` no longer owns its own waiter
list, there's no reason left for it to expose two overlapping ways to
acquire it.

**#67 first.** `est::mutex` used to be its own hand-written copy of
exactly the pieces `est::counting_event<Mode>` already has: an intrusive
waiter list, a `ready_node`-derived resume node (`lock_resume_node`/
`acquire_resume_node`), `abandon()` completing an abandoned waiter with
an exception rather than silently dropping it. The mapping onto
`counting_event<Mode>` turned out exact, not approximate:
`binary_event<EventResetMode::automatic>` (max_count = 1) already has
"unlocked = one unit available, locked = none available," and automatic
mode's own `set()` - hand the freed unit directly to the next queued
waiter, never reading as free in between - is already precisely the
handoff semantics `unlock()` needs. `mutex` is now nothing but:

```cpp
class mutex {
  ...
  [[nodiscard]] auto lock() -> future<lock_guard> {
    if (event_.try_wait()) {
      return make_ready_future<lock_guard>(*this);
    }
    return event_.wait().then([this] { return lock_guard(*this); });
  }
private:
  void unlock() noexcept { event_.set(); }
  binary_event<EventResetMode::automatic> event_;
};
```

`counting_event<Mode>` gained one new method for this, `try_wait()`:
the synchronous half of `wait()` (the `count_ > 0` check plus the
automatic-mode decrement), split out on its own so a caller wanting the
fast path without a `future<void>` it would immediately discard - exactly
`mutex::lock()`'s own situation - doesn't have to build and throw one
away. `wait()` itself was refactored to call it, rather than duplicating
the check.

**Then #66.** `lock()` (a plain `future<void>`) and `acquire()` (a
`future<lock_guard>`) collapsed into one method, `lock() ->
future<lock_guard>`: every call site in this codebase already wanted the
RAII guard, so the lower-level pair only ever added a way to forget the
matching `unlock()` call. `unlock()` itself moved to `private` -
`lock_guard` (a nested class, sharing its enclosing class's access) is
the only remaining caller, so dropping the guard `lock()` returns is now
the *only* way to release a lock this class exposes at all.

**The allocation-count tradeoff, made explicit rather than silently
accepted.** `lock()`'s fast (uncontended) path costs exactly what it
always did - one allocation, the returned `future_state<lock_guard>`
itself - because it goes through `try_wait()` directly rather than
`wait()` (which would otherwise build and discard a `future<void>` just
to build a second, different future in its place). The *contended* path
is genuinely more expensive than the hand-written version it replaced:
`event_.wait().then(...)` needs `event_.wait()`'s own
`future_state<void>` + `event_resume_node`, *plus* `.then()`'s own
downstream `future_state<lock_guard>` + `concrete_continuation<Fn,
lock_guard>` node to turn that `future<void>` into the `future<lock_guard>`
`lock()` returns - four allocations where the old, purpose-built
`acquire_resume_node` needed two. Accepted deliberately: the alternative
was re-implementing the waiter queue, resume node, and
abandonment-completion machinery a second time, exactly what #67 asked
to stop doing, for a cost that only a *contended* `lock()` call - already
the slow path - ever pays.

**Issue #46 (the dangling-`mutex&`-in-deferred-completion hazard) is
untouched by this, not fixed by it.** The `.then()` callback,
`[this] { return lock_guard(*this); }`, still captures a raw `mutex*` and
still only actually runs once `est::loop` drains the node some time
after `unlock()`/`event_.set()` enqueued it - the identical "needs the
mutex still alive once the deferred completion runs" precondition
`acquire_resume_node::run()` used to carry, now living in a different
node. #46 stays open, tracking that gap wherever it ends up living next.

**A real, previously-undiscovered leak surfaced by this refactor, and
fixed as part of it.** The first version of the contended-path test
("destroying a mutex with a coroutine co_await-ing lock() still pending
leaks nothing") failed: 8 allocations, 4 deallocations. Root cause:
`future_state<T>::concrete_continuation<Fn, U>` (and its sibling,
`detail::flatten_forwarder<T>`) had no `abandon()` override at all -
`ready_node::abandon()`'s default no-op body left `downstream_` silently
dropped on abandonment instead of completed - already flagged in both
classes' own doc comments as "a real, separate question this class
doesn't yet answer," but never actually exercised by any existing test,
since nothing before this refactor `co_await`-ed a `.then()`-chained
future whose *upstream* future_state could be torn down out from under
it. `mutex::lock()`'s slow path is exactly that shape:
`event_.wait().then(...)`, with a coroutine `co_await`-ing the result.
When the mutex (and its `event_`'s underlying `future_state<void>`) were
destroyed with that coroutine still suspended, the abandoned
`concrete_continuation` node's default `abandon()` left `downstream_`
uncompleted - stranding the coroutine's frame with nothing left to free
it, exactly the "neither side can free the other first" hazard every
other resume node in this codebase (`future_resume_node<T>`,
`detail::event_resume_node`, this class's own former
`lock_resume_node`/`acquire_resume_node`) already defends against with a
real `abandon()` override.

Fixed in `est/src/future.cppm`: both classes now override `abandon()` to
complete `downstream_` with an exception, matching every other resume
node's own convention. Fixed symmetrically in `flatten_forwarder<T>` too,
even though nothing had yet triggered *that* copy of the same bug - it
was the same open question, the same fix, and leaving one half fixed
while the other stayed silently broken would just be waiting for the
next real trigger. A new, targeted regression test for the
`flatten_forwarder<T>` side ("dropping an abandoned inner future_state
completes the flattened future, no leak", `est/tests/future_tests.cpp`)
was added alongside it, since nothing else in the existing suite
exercised that specific path either.

Docs rewritten to match, and restructured (`counting_event<Mode>` now
described before `mutex`, since the dependency direction flipped):
`docs/wiki/Coroutines.md` (the old "`est::mutex::lock()` becomes
awaitable" section split into a `counting_event<Mode>`-first section plus
a new, much shorter "`est::mutex`: `lock()` built on top of
`binary_event<automatic>`" one), `docs/wiki/Architecture.md` (dependency
graph edge flipped from `event --> mutex`-shaped duplication to
`mutex --> event`, prose rewritten), `docs/wiki/Home.md` (five-second
summary and the source-location table), `docs/wiki/Loop-And-Timers.md`
and `docs/wiki/Continuation-Node-Mechanism.md` (passing mentions of
`est::mutex`'s "own" waiter list, now `est::counting_event<Mode>`'s),
`docs/wiki/Global-Lookup-Codegen.md` (one stale claim about which
methods resolve `current_loop()`, predating this refactor but caught
while touching the same paragraph). `examples/probe/main.cpp`'s two
mutex probes were also reshaped: `probe_mutex_lock()` now takes an
`est::mutex&` parameter rather than a local mutex (a local would dangle -
`lock()`'s returned `future<lock_guard>` holds a `mutex*` now, unlike the
old `future<void>`), and `probe_mutex_unlock_uncontended()` (which called
the now-private `unlock()` directly) became `probe_mutex_lock_guard_drop()`,
probing the release path through a dropped `lock_guard` instead.

`est/tests/mutex_tests.cpp` rewritten throughout: every `acquire()` call
site renamed to `lock()`; every direct `mutex_ref.unlock()` call replaced
with capturing the guard `co_await mutex_ref.lock()` returns into a local
and letting it drop via scope exit (a coroutine finishing, or an inner
block ending) instead; the old lock()-specific "destroying a mutex with a
coroutine still queued on lock() leaks nothing" test dropped as now
redundant with its acquire()-based twin (same code path, now that the
two methods are one), and "unlock() releases the lock when nothing is
waiting" folded into "dropping the lock_guard unlocks the mutex" (same
scenario, the latter already covering it once `unlock()` can no longer
be called directly). Net test count: 14 before, 12 after - not a
coverage loss, since both removed cases were testing scenarios a
remaining test already covers via the now-unified API.

**Verified in the pinned Docker devenv:** 173/173 tests pass (one new,
the `flatten_forwarder<T>` abandonment regression test above);
`clang-format`/`clang-tidy` clean; 140/140 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 100% (103/103 changed lines covered).

### Issue #77: shared `detail::promise_resume_node`, replacing `yield_resume_node`/`event_resume_node` (done)

A follow-up from PR #62's own review: `mutex::lock_resume_node` (already
gone by the time this issue was picked up - `est::mutex` is built
directly on `est::counting_event` since issue #67, above),
`est:promise`'s `yield_resume_node`, and `est:sync.event`'s
`event_resume_node` were three separate `ready_node`-derived classes,
byte-identical apart from the fixed exception message each passed its
own `abandon()`: a plain `promise<void>` member, `run()` calling
`promise_.set_value()`, `abandon()` completing that same promise with a
`std::runtime_error` instead of silently dropping it. Issue #77 asked to
collapse whichever of these were still separate into one shared type;
by the time this was picked up, only `yield_resume_node` and
`event_resume_node` remained (the mutex's own copy having already been
deleted outright by #67, not merged into anything), so those two were
the actual scope.

Collapsed into `est::detail::promise_resume_node` (`est/src/promise.cppm`,
below `sleep_resume_node`), taking the fixed message as a constructor
argument (a `std::string_view`, not a `std::string` - every call site
passes a string literal with static storage duration, so there's nothing
to own or copy; `std::runtime_error`'s own constructor is what actually
allocates a copy, and only once `abandon()` is ever reached at all).
Defined in `est:promise` rather than `est:sync.event` (where
`event_resume_node` used to live) because of the module partition
direction: `:promise` sits *below* `:sync.event` in the DAG
(`:sync.event` already `import`s `:promise`), so this is the only
placement that needs no new dependency edge - `est:sync.event`'s
`wait()` reuses a type its own module already imports, rather than
`est:promise`'s `yield_execution()` needing to import `:sync.event` (the
wrong direction) to reuse a type defined there instead. Not nested inside
`est::mutex` or `est::counting_event<Mode>` either, for the same reason
neither predecessor was: `run()`/`abandon()` never touch anything about
whichever type enqueued the node, so one free class in `est::detail`
serves every caller instead of minting an identical type per caller (or,
now, per `Mode` instantiation).

`yield_execution()` and `counting_event<Mode>::wait()`'s slow path both
constructed a `detail::promise_resume_node` directly, each supplying its
own message ("loop destroyed while yield_execution() was pending",
"counting_event destroyed while wait() was pending" - the same two
messages the two deleted classes already used, unchanged). `mutex::lock()`
inherited this transparently, through `event_.wait()`.

This first pass surfaced one thing: a `promise_resume_node::abandon()`
built from a `std::string_view` member (rather than a string literal
baked directly into the `std::runtime_error(...)` call, as
`sleep_resume_node::abandon()` still does) trips `clang-tidy`'s
`bugprone-exception-escape` - `std::string(string_view)`'s own allocation
could in principle throw `std::length_error`/`bad_alloc` inside a
function declared `noexcept`. Suppressed with a `NOLINTNEXTLINE`, the
same "intended fail-fast on a precondition violation, not something to
route around" reasoning `future_state<T>::get_exception()`'s own
pre-existing `NOLINT` (`est/src/future.cppm`) already documents for an
unrelated noexcept function in this codebase - not a new class of risk,
just a different function taking the same accepted trade.

**Revised during PR review** (before merge - the PR stayed open for
this): the per-call-site message was never actually load-bearing.
Nothing anywhere in this codebase inspects `what()` to tell one
abandonment apart from another - a completed promise's exception is only
ever observed as "this failed," never matched against particular text -
so three-plus call sites each hand-rolling their own `std::runtime_error`
literal (`sleep_resume_node`, `promise_resume_node`,
`flatten_forwarder<T>`, `concrete_continuation<Fn, U>`) was needless
duplication one level up from the one issue #77 had already removed.
Introduced `est::detail::abandoned_exception` (`est/src/loop.cppm`, next
to `ready_node`/`timer_node` - the lowest-level module every one of these
call sites' own modules transitively imports, so no new dependency edge
either): a fixed, message-less `std::runtime_error` subclass, default-
constructible, that every one of those four `abandon()` overrides now
throws instead of building its own literal. This also made the earlier
`NOLINTNEXTLINE(bugprone-exception-escape)` unnecessary: with the
message gone, `abandoned_exception()`'s own constructor passes a plain
string literal straight to `std::runtime_error`, the same shape
`sleep_resume_node::abandon()` always used and clang-tidy never flagged -
removed along with the `std::string_view abandoned_message_` member it
was suppressing a false positive for.

`promise_resume_node` also picked up a template parameter,
`promise_resume_node<T>`, matching `est:future`'s own
`future_resume_node<T>`/`concrete_continuation<Fn, U>` convention - both
of its current instantiations are `promise_resume_node<void>`
(`run()`'s `promise_.set_value()` call, with no argument, only compiles
against `promise<T>` for `T = void` in the first place, so nothing else
could instantiate this class regardless), but there's nothing left in
`run()`/`abandon()` that's actually void-specific once the message is
gone either.

No behavior change beyond the exception's own (now generic) `what()`
text: `run()`/`abandon()` do exactly what the four call sites' own
copies did, just through one shared type instead of four (or, for
`promise_resume_node<T>`, two) separate ones. Docs updated to match
throughout - `docs/wiki/Coroutines.md` (the `event_resume_node` section
renamed and rewritten to describe the shared, templated type, cross-
referenced from `yield_execution()`'s own section instead of duplicating
the explanation), `docs/wiki/Loop-And-Timers.md`,
`docs/wiki/Allocation-Patterns.md`, `docs/wiki/Continuation-Node-Mechanism.md`
(a pre-existing, unrelated staleness caught while touching the same
`flatten_forwarder<T>::abandon()` code sample - the surrounding prose
still claimed neither `flatten_forwarder<T>` nor `concrete_continuation<Fn,
U>` overrode `abandon()` at all, contradicted by its own code block right
above it), `docs/wiki/Home.md` (source-location table: `promise_resume_node<T>`
moved from `event.cppm` to `promise.cppm`, `abandoned_exception` added
against `loop.cppm`), plus stale references in `est/tests/loop_tests.cpp`
and `est/tests/mutex_tests.cpp` comments.

**Verified in the pinned Docker devenv:** 173/173 tests pass under both
the `default` and `ci` presets (pure refactor - no new test needed,
existing coverage of every call site already exercises `run()`/
`abandon()` on the shared types the same way it did on the separate
ones); `clang-format`/`clang-tidy` clean (zero `NOLINT` needed for this
code, unlike the pre-review version); 140/140 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 100% (11/11 changed lines covered).

---

### `shared_ptr<T>::count()`, issue #64 & #71: `then()` moves instead of copying when it's the last owner

Three pieces, requested and sequenced in that order: a public `count()`
accessor on `est::shared_ptr<T>` first, then issue #64 built on top of it,
then issue #71 - which the ref-count check in #64 makes genuinely a
one-line addition.

**`shared_ptr<T>::count()`** (`est/src/util/shared_ptr.cppm`, both the
primary control_block-based template and the `ref_counted`-based
intrusive specialization): returns how many `shared_ptr<T>` instances
currently share this object's control block/ref count - 0 for an empty
or moved-from `shared_ptr`. No new state; both specializations already
carry the ref count `reset()`/copy already maintain, just not previously
exposed.

**Issue #64.** `future_state<T>::concrete_continuation<Fn, U>::run()`'s
unwrapped, non-void, non-failed branch used to always call `state.get()`
(the non-consuming `const T&` overload), even when nothing else was left
to observe the future_state afterward. It now checks
`this->owner_.count() == 1` first - `owner_` is this node's own
`shared_ptr<future_state<T>>`, bound via `bind_owner()`; if the count
comes back 1, this node is the only reference left (no live
`future`/`promise` handle, no sibling continuation), so
`std::move(state).get()` is used instead, turning what would otherwise
be a copy (for an `Fn` taking `T` by value) or a harmless no-op
distinction (for `const T&`) into an actual move. Gated on
`std::invocable<Fn&, T&&>` at compile time (an `if constexpr`, checked
before the runtime `count()` branch): a purely lvalue-binding callback
like `[](auto& value) {...}` - invocable with `const T&` (which is what
`then_callback_for`/`invocable_unwrapped` already require) but not with
`T&&` - stays on the plain `state.get()` path unconditionally, since it
means to observe the value in place, not receive a moved-from one.

**Issue #71.** `future<T>::then(Fn&&)` split into two ref-qualified
overloads - `&` (the original behavior, unchanged) and a new `&&`
overload that moves this handle's own `shared_ptr<future_state<T>>` into
a local before delegating to `future_state<T>::then()`, dropping this
handle's reference immediately rather than leaving it held until `*this`
goes out of scope. A member function can't mix a ref-unqualified
overload with a ref-qualified one of the same signature (a real, "class
member cannot be redeclared" compile error, not just an overload-
resolution ambiguity) - hence marking the existing overload `&` rather
than leaving it unqualified. Every existing call site is unaffected:
lvalue calls still resolve to `&`, rvalue calls (including a plain
temporary, already an rvalue) to `&&`, exactly as before from the
caller's perspective. The net effect: `std::move(future).then(fn)`
(or a temporary's `.then(fn)`) makes `owner_.count() == 1` come back true
one reference sooner, letting issue #64's move path fire in more cases -
e.g. a `.then()` chain where nothing else ever held onto an intermediate
`future<T>`.

**A real compile-time subtlety surfaced (and fixed) during
verification.** `std::exchange(state_, nullptr)` doesn't compile for the
`&&` overload's implementation - `shared_ptr<T>` has no implicit
conversion from `nullptr_t`, so `state_ = nullptr` (what `std::exchange`
needs to do internally) is ill-formed. Replaced with an explicit
move-construct into a local (`auto state = std::move(state_);`) instead,
which correctly empties `state_` via `shared_ptr`'s own move constructor.

**Docs updated to match:** `docs/wiki/Continuation-Node-Mechanism.md`'s
"The two calling conventions" code excerpt and prose (the move branch
added to the `run()` snippet, with the `invocable<Fn&, T&&>` gate
explained); `docs/wiki/Coroutines.md`'s "Why `future<T>` can't be
copyable" section, which previously described `.then()`'s unwrapped
dispatch as "always-copying" and stated outright that the general
"move if you're the last owner, copy otherwise" pattern "doesn't have a
sound place to hook into this specific design" - both no longer true.
The corrected version explains why the pattern *does* work for `.then()`
(the check and the consumption happen together, synchronously, inside
the same node's own `run()`, on the very `shared_ptr` that call is about
to read through) but still can't work for `get()`/`await_resume()` (the
object making the "am I sole owner" decision and the object guaranteed
to hold a reference at that moment - the resume node driving the
resumption - aren't the same `shared_ptr`).

**Tests added:** three `shared_ptr_tests.cpp` cases for `count()` itself
(tracks copies/drops for both specializations; 0 for empty/moved-from);
three `future_tests.cpp` cases using a new `copy_move_tracker` helper
(counts copy- vs move-constructions, inheriting the running count from
its source) - the negative case (another handle still shares the
future_state, so the callback's by-value parameter is copy-constructed),
issue #64's positive case (promise and future both confined to an IIFE
and dropped before the loop drains, so the node is the sole owner by the
time it runs), and issue #71's own isolated contribution (promise
dropped explicitly, but the future handle deliberately kept alive
through `run_until_idle()` - proving the `&&` overload's explicit
release matters, not just the temporary's natural lifetime).

**Verified in the pinned Docker devenv, from a from-scratch build (no
cached BMIs, to rule out stale build state):** 179/179 tests pass (six
new: three for `count()`, three for #64/#71); `clang-format`/`clang-tidy`
clean; 146/146 tests pass under the `sanitize` preset (ASan+UBSan) too;
`diff-cover` coverage gate against `main` at 100% (121/121 changed lines
covered).

### Issue #65: `then_fast()` - an opt-in fast path matching `co_await`'s (done)

`co_await` on an already-ready `future<T>` resumes inline
(`future_awaiter<T>::await_ready()`, `est:future`), but `.then()`
registered on an already-ready `future_state<T>` always deferred through
`est::loop`'s ready-queue regardless - a "subtly different," not
obviously intentional, asymmetry between the two ways of consuming a
future. Two designs were sketched before picking one:

- **A separate `fast_future<T>` type**, distinct from `future<T>`, whose
  own `.then()`/`co_await` always run inline when ready, with a
  one-directional `fast_future<T> -> future<T>` conversion for widening
  back to the safe default. More self-documenting (a value's type alone
  says whether its continuations can run inline), but real cost: reusing
  `future<T>`'s own `get()`/`clone()`/`promise_type` and, especially,
  `future_awaiter<T>` (currently built around a `future<T>&`, not a
  `future_state<T>&`/`shared_ptr` directly) would need a real refactor of
  already-carefully-commented, already-tested code, not just a new class
  bolted on alongside it.
- **An opt-in flag on the existing registration path** - chosen. No new
  type; `future_state<T>::set_continuation()` gains a
  `bool run_inline_if_ready = false` parameter, and `then()`
  grows a sibling, `then_fast()`, that passes `true` through one shared
  private `then_impl()`. `future<T>::then_fast()` forwards to it with the
  identical `&`/`&&`-qualified split `then()` already has (issue #64/#71).

**Mechanically:** `set_continuation()`'s already-ready branch, on
`run_inline_if_ready`, runs `node.run()` and lets it be deleted right
there (originally a bare `delete &node;`; later replaced with a local
`unique_ptr<continuation_node>` - see the follow-up entry below) instead
of `current_loop().enqueue_ready(node)` - the same "run, then delete"
idiom `est::loop`'s own `run_one()`/`destroy_guard()` use on its own
drain pass, just performed directly since those two helpers are private
to `est::loop`. The not-yet-ready branch (`waiters_.enqueue()`)
is completely untouched: nothing is stored on the node itself, so
whether a given `then_fast()` call ends up running inline or deferred is
decided once, at registration time, by whether the future was already
ready then - a `then_fast()` call on a not-yet-ready future_state defers
exactly like `then()` always has, since there's nothing to run inline
yet. `then()`'s own default stays unconditionally deferred - it must
remain safe for a chain of any length (deferring every completion
through `est::loop` turns unbounded chain length into an iterative drain
instead of direct recursion on the call stack), the same invariant this
codebase already applies to `mutex::unlock()`/`counting_event<Mode>::set()`.
`then_fast()` is the explicit, per-call opt-in for a caller who knows
their own callback is cheap and specifically wants the loop round-trip
skipped, accepting the same recursion-depth trade a coroutine's
already-ready `co_await` always implicitly did. The monadic-flatten path
(`fulfill()`/`flatten_forwarder<U>`) deliberately keeps deferring
regardless of `then_fast()` - "fast" doesn't thread through flattening,
keeping the change to one call site's behavior rather than a second
property every future-returning path has to carry.

**A real, non-obvious cost surfaced while writing the tests: `then_fast()`'s
own `&&` overload can't actually get issue #64's move optimization.**
`concrete_continuation<Fn, U>::run()`'s `owner_.count() == 1` check only
ever reaches 1 for `then()` because `run()` happens *later*, once
`est::loop` drains it - by which point whatever temporaries were on the
registering call's own stack (including `future<T>::then(Fn&&) &&`'s own
`state` local) have already unwound. `then_fast()`'s `run()` happens
synchronously, *inside* that same call - its own `state` local is still
alive, holding a reference, at the exact moment `run()` checks
`owner_.count()`; that reference plus `owner_` itself already puts the
count at 2, never 1, regardless of how carefully a caller manages every
other handle. Confirmed by literally reproducing issue #64's own
sole-owner test shape with `then_fast()` instead of `then()`: it copies,
not moves - the opposite of `then()`'s own outcome under the identical
shape. `future<T>::then_fast(Fn&&) &&` is kept anyway, for API-shape
symmetry with `then()` and because releasing a handle early is never
harmful - it just isn't what makes the difference here the way it does
for `then()`. Documented as such at both the `&&` overload's own doc
comment and the regression test that demonstrates it
(`future_tests.cpp`, *"then_fast()'s inline run() defeats the sole-owner
move optimization then() relies on"*).

**Docs updated to match:** `docs/wiki/Continuation-Node-Mechanism.md`
(a new "`then_fast()`: running inline instead of deferring" section, plus
a cross-reference from "What `then()` actually builds" and an updated
"Parent already ready" bullet under "Node lifecycle"),
`docs/wiki/Coroutines.md` (the `await_ready()` passage that used to argue
"the two aren't inconsistent" now points to `then_fast()` as the actual
resolution, plus a note on `counting_event<Mode>::wait()`'s own
already-signaled fast path).

**Tests added (`future_tests.cpp`):** `then_fast()` running inline on an
already-ready future with no `run_until_idle()` needed; deferring
identically to `then()` when registered before `set_value()`;
propagating a stored exception inline; and the sole-owner/move-defeated
case above.

**A related hardening, caught while auditing whether `then_fast()`'s new
inline `node.run()` call could let an exception surface somewhere more
surprising than before.** Every `run()`/`fire()` in the codebase either
holds no throwing user code at all (`promise_resume_node<T>::run()`,
`sleep_resume_node::fire()` - plain `set_value()`), delegates to
compiler-generated coroutine unwind semantics that already redirect an
exception to `unhandled_exception()` before it ever reaches the caller
(`future_resume_node<T>::run()`), or - the one node that runs arbitrary
caller-supplied code - wraps that call in a catch-all
(`concrete_continuation<Fn, U>::run()`, `future.cppm`). `check()`/
`assert_failure()` failures were already ruled out as a throw path
regardless: `platform::interface::assert_failure()` is declared
`noexcept`, so a failing precondition terminates the process rather than
unwinding through anything.

One real asymmetry surfaced, though: `flatten_forwarder<T>::run()`'s
`state.failed()` branch was an early `return` *ahead of* its own `try`,
unlike `concrete_continuation<Fn, U>`'s identical branch, which was
always inside its own `try`. Nothing on that path currently throws
(`get_exception()` is `noexcept`, `set_exception()` itself doesn't throw
short of `bad_alloc` or a `check()`-triggered terminate), so this wasn't
a live bug - but it meant an exception there would have escaped `run()`
uncaught instead of being routed into `downstream_`'s own
`set_exception()`, the one thing every other node with user-facing
failure handling already guarantees. Fixed by folding the `failed()`
check into the existing `try`, matching `concrete_continuation<Fn, U>`'s
shape exactly - no behavior change under any currently-throwing path,
just removing the one place structurally relying on nothing ever
throwing there. No new test: forcing this specific branch to throw would
need mocking allocation failure or a `check()` violation, neither of
which this codebase currently does.

**Verified in the pinned Docker devenv:** 183/183 tests pass (four new);
`clang-format`/`clang-tidy` clean; 150/150 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 100% (73/73 changed lines covered).

### `std::unique_ptr` replacing hand-written node-guard `new`/`delete` (done)

A follow-up design question, not tied to a numbered issue: now that every
concrete `ready_node`/`timer_node` already has its own `operator new`/
`operator delete` (`detail::current_allocator_new_delete<T>`,
est:util.current_loop), could the remaining hand-written `new`/`delete`
call sites - `loop::destroy_guard()` (a `scope_exit`-wrapped `delete`),
`detail::abandon_ready_node()`/`abandon_timer_node()`
(`node.abandon(); delete &node;`), and `future_state<T>::set_continuation()`'s
new `then_fast()` inline branch (`node.run(); delete &node;`, added just
above) - be replaced with `std::unique_ptr` instead, and at what cost?

**Checked the assembly cost empirically before touching any code**, the
same way `docs/wiki/Global-Lookup-Codegen.md` already validates a
different codegen claim in this codebase: compiled a minimal
reproduction of the actual shape (a `ready_node`-style virtual base with
a virtual destructor and a derived type with a custom `operator new`/
`delete`) at `-O2` with the pinned clang, comparing a bare
`delete &node;` through the base reference against
`std::unique_ptr<ready_node>`'s destructor doing the same, and a bare
`new concrete()` against `std::make_unique<concrete>()` followed by
`.release()`. Both pairs produced byte-for-byte identical assembly.
`delete` through a base reference with a virtual destructor already
tail-calls into the vtable's deleting-destructor slot - exactly what
`unique_ptr<ready_node>`'s own destructor compiles down to - and
`make_unique<T>` still resolves to `T`'s own custom `operator new` (name
lookup finds it before the global one), so there's nothing to devirtualize
or deoptimize either way: every call site that actually deletes a node
here has already type-erased it down to `ready_node&`/`timer_node&` by
the time it gets there (out of `intrusive_list<ready_node>` or
`pending_timers_`, both deliberately non-owning, type-erased
containers), so `unique_ptr` carries exactly the same missing static-type
information a raw pointer does - there's no call site where switching
would let the compiler skip the vtable indirection that isn't already
skipping it today.

**Changed anyway, for the style win alone** (fewer naked `delete`
expressions, no assembly cost either way):

- `detail::abandon_ready_node()`/`abandon_timer_node()`
  (`est/src/loop.cppm`): construct a local `const std::unique_ptr<...>`
  first, then call `abandon()` through it - ownership transfers before
  `abandon()` runs, but the implicit deletion still only happens once the
  function returns, preserving the exact `abandon()`-before-`delete`
  ordering the old two-statement body had.
- `future_state<T>::set_continuation()`'s inline branch
  (`est/src/future.cppm`, `then_fast()`'s own mechanism, added just
  above): same pattern - a local `unique_ptr<continuation_node>` owns
  `node` before `run()` is called through it.
- `loop::destroy_guard()` (`est/src/loop.cppm`): first changed to return
  `std::unique_ptr<Node>(&node)` directly instead of a
  `scope_exit`-wrapped lambda, then removed outright once that body
  shrank to exactly that one line - a template wrapping a single-line
  `unique_ptr<Node>(&node)` construction stopped earning its keep over
  just writing it inline at both of its two call sites
  (`run_one()`/`fire_ready_timers()`), which is what each now does
  directly instead of naming a shared helper for it. One further
  consequence surfaced by `clang-tidy`, not anticipated going in:
  `run_one()` no longer touches any member of `*this` at all once the
  call to `destroy_guard()` (a non-static member, needing an implicit
  `this` just to be called) is gone -
  `readability-convert-member-functions-to-static` caught it immediately,
  fixed by marking `run_one()` `static`.

Docs updated to match: `docs/wiki/Coroutines.md` (the `run_one()` code
excerpt and surrounding prose, which quoted the old `scope_exit`-based
`destroy_guard` and a literal `delete &node`),
`docs/wiki/Continuation-Node-Mechanism.md` (the `set_continuation()` code
excerpt in the `then_fast()` section), `docs/wiki/Loop-And-Timers.md`
(`destroy_guard`'s own description, now describing `run_one()`'s inline
guard directly), `docs/wiki/Allocation-Patterns.md`, and the doc comments
on `ready_node`/`timer_node` themselves (no longer claiming deletion is
"through a plain `delete`" specifically, since it's now spelled two
different ways depending on the call site - the actual safety argument,
resolving through the dynamic type's own vtable slot regardless of which
spelling triggers it, is unchanged and still the point being made).

**Verified in the pinned Docker devenv:** 183/183 tests pass;
`clang-format`/`clang-tidy` clean; 150/150 tests pass under the
`sanitize` preset (ASan+UBSan, the most relevant check here - it would
have caught a double-free or use-after-free from a botched ownership
handoff immediately) too; `diff-cover` coverage gate against
`origin/main` (this PR having since been retargeted there directly, its
original stacked base already merged) at 100%
(9/9 changed lines covered).

---

### Issue #54: `est::when_all()` - waiting on more than one future at once

Scoped down from the issue's full proposal (`when_all` + `when_any`,
each in a fixed-arity and a range-based form) after discussing it: this
pass covers only `when_all`, both arities; `when_any` is left for a
follow-up issue, since it has its own open question (what happens to the
futures that haven't completed yet, with no cancellation mechanism to
stop them) worth deciding separately rather than folded into this one.

**Failure semantics, resolved.** The issue's own "open questions"
section asked whether `when_all` should fail fast (`Promise.all`-style)
or wait for every input regardless (`Promise.allSettled`-style).
Resolved here as neither, exactly: the input futures are owned by the
caller (passed as `future<T>&`, never consumed), `when_all()` waits for
every one of them to complete - success or failure, no fail-fast - and
then simply resolves; it never fails itself, and never reads a value or
an exception out of any input. The caller checks `failed()`/`get()` on
whichever inputs it cares about afterward, exactly as if it had awaited
each one individually. This sidesteps the "what does the combined result
look like when one of N heterogeneous types failed" question entirely -
there's no combined value or exception to construct in the first place,
just a `future<void>` signaling "everyone's done now."

**Shape:** a new partition, `est::when_all()` (`est/src/when_all.cppm`,
`:when_all` - sits at the bottom of the DAG next to `:sync.mutex`,
depending on `:future`/`:promise`/`:util.shared_ptr`/`:util.current_loop`
and nothing else depends on it). Two overloads:
- `when_all(future<Ts>&... futures) -> future<void>` - fixed arity,
  heterogeneous. An empty pack resolves immediately.
- `when_all(std::span<future<T>> futures) -> future<void>` - a
  dynamically-sized, homogeneous run instead of a fixed argument list.
  An empty span resolves immediately. Template argument deduction can't
  see through a container's implicit conversion to `std::span`, so a
  caller passing, say, a `std::vector<future<T>>` needs to spell out
  `std::span(the_vector)` at the call site.

**Mechanism:** a small counting barrier. `detail::when_all_state` holds
a `promise<void> result` and an `int remaining`, allocated once via
`detail::when_all_setup()` (shared by both overloads) using
`shared_ptr<when_all_state>::make(current_allocator(), ...)` - `est::shared_ptr`,
not `std::shared_ptr` - allocator-first, per `CLAUDE.md`. A `count == 0`
call resolves the returned future immediately, skipping the allocation
entirely. Each input gets one `detail::when_all_track<T>()` registration,
which decrements `remaining`; whichever one brings it to zero calls
`result.set_value()`.

**Two real correctness bugs caught during verification, not just
clean-room design** - both found by a test written specifically to
exercise the failure mode, not by inspection.

*Bug one, caught by this pass's own tests before ever opening the PR.*
The obvious per-future hook shape - a single generic `[](auto& completed)
{...}` lambda, reused for every `T` in the pack - silently breaks
`when_all()` for any `T` where a constituent future fails.
`then_callback_for<T>`'s own dispatch (`invocable_unwrapped<Fn, T>()`,
`docs/wiki/Continuation-Node-Mechanism.md`) checks unwrapped mode first,
and a generic lambda satisfies that check too - a template parameter
binds to `const T&` exactly as readily as to `future<T>&` - so `then()`
picks unwrapped mode, which auto-propagates a failure straight to the
(here, discarded) `.then()`-returned future *without ever calling the
lambda at all*. A failed input would simply never decrement `remaining`,
hanging `when_all()`'s returned future forever the first time any one
constituent failed - caught by the "counts a failed future the same as a
succeeded one" test below. Fixed by typing the hook explicitly on
`future<T>&` (not a generic parameter) - a `future<T>&` parameter can't
bind a `const T&` argument, so `invocable_unwrapped` is false regardless
of `T` (`T = void` included, where the zero-argument unwrapped check
fails for the identical reason: such a hook always takes exactly one
argument), forcing wrapped mode - always invoked, success or failure -
unconditionally.

*Bug two, caught by a code review pass after the PR was already open.*
Even with bug one fixed, a *correctly-typed* hook registered *directly*
on an input future is still never invoked if that future's own
`future_state` is *abandoned* (destroyed while still pending) rather
than actually completed - a concrete, plausible trigger: a helper
function starts some async producer, calls `when_all()` on the future it
hands back, and returns, letting its own local promise/future pair for
that one input go out of scope once nothing local needs them any more.
`concrete_continuation<Fn, U>`'s own `abandon()` override (Issue #66 &
#67, above) unconditionally completes *its own* downstream with an
exception - it never invokes `fn_` at all - so a counting hook living
directly in `fn_` would simply never run for that input, hanging
`when_all()`'s returned future forever exactly like bug one, just from a
different precondition (previously described here, incorrectly, as
"nothing new needed for abandonment-safety" - that was verified only for
memory-safety, i.e. no leak on full teardown, never for whether the
returned future actually resolves when just one input is abandoned while
the loop and every other input keep going). Fixed by replacing the
single per-input hook with a two-stage `.then()` chain
(`detail::when_all_track<T>()`): a no-op first stage, typed on
`future<T>&` for the identical bug-one reason, whose only job is to
produce a `future<void>` that reliably completes whenever the input does
- *whichever way that happens*, since `abandon()`'s own
exception-completion goes through `future_state<T>::complete()` exactly
the same way a normal completion does, waking up second-stage waiters
identically either way. The real counting logic lives in that second
stage instead, also typed on a concrete `future<void>&`, and so always
runs exactly once per input, completed or abandoned.

Full writeup of both traps and the two-stage fix in
`docs/wiki/Continuation-Node-Mechanism.md`'s "`est::when_all()`: forcing
wrapped mode, and surviving abandonment" section.

**Docs:** `docs/wiki/Home.md`'s source-location table; a new `:when_all`
node and edges in `docs/wiki/Architecture.md`'s dependency graph, plus a
short paragraph on why it sits where it does; both traps and the
two-stage fix written up in full in
`docs/wiki/Continuation-Node-Mechanism.md`.

**Tests added** (`est/tests/when_all_tests.cpp`): empty pack resolves
immediately; resolves only once every input is ready, not before;
resolves immediately when every input is already ready; counts a failed
future the same as a succeeded one (caught bug one above); still
resolves when one input is abandoned while the loop keeps running
(caught bug two above, added after the code-review pass that found it);
works across heterogeneous types including `future<void>`; does not
consume the caller's futures; the `std::span` overload's own
ready-timing and empty-range cases; two leak tests (shared state and
hooks freed on the happy path, and freed even when every input is
abandoned before completing).

**Verified in the pinned Docker devenv:** 190/190 tests pass (11 new);
`clang-format`/`clang-tidy` clean; 157/157 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 98% (changed lines covered - the handful of uncovered lines
are `when_all_tests.cpp`'s own `counting_resource::do_is_equal()`, copied
boilerplate never exercised by any of these tests, same as in every
other test file that defines one).

**Follow-up from PR review: `then_fast()` + `one_shot_event`, and a
third bug this surfaced.** Two review comments on PR #89, after
`then_fast()` (issue #65) landed on `main` from a parallel PR opened
after this one: `when_all_track()` should use `then_fast()` at both
stages instead of `then()`, and `when_all_state` should hold a
`one_shot_event<EventResetMode::manual>` instead of a raw
`promise<void>`.

*`then_fast()` at both stages.* `when_all_track()`'s two callbacks - a
no-op and a two-line decrement - are exactly the "caller specifically
knows `fn` is cheap" case `then_fast()`'s own doc comment describes, the
same one `counting_event<Mode>::wait()`'s own already-signaled fast path
already relies on. Swapping both `then()` calls for `then_fast()` lets
an already-ready input's entire two-stage chain resolve synchronously,
with no loop round trip, instead of always deferring.

*`one_shot_event<manual>` instead of `promise<void>`.* `event.wait()`
now produces the `future<void>` `when_all()` hands back directly, with
no separate `make_promise_future()` call; Mode = manual matches the
"one broadcast, however many observers" shape `when_all_state`'s
completion signal actually has.

*Bug three, caught by this pass's own rewritten test, not review.*
Naively building `when_all_state` and calling `event.wait()` immediately
- before registering any `when_all_track()` calls - defeated the entire
point of switching to `then_fast()`: `wait()`'s own already-signaled
fast path (`try_wait()`) can only take effect if the event is *already*
`set()` at the moment `wait()` is called, but calling `wait()` first
means every `when_all()` call registers a waiter against a still-
unsignaled event unconditionally - even one where every input was
already ready and resolved synchronously moments later, inside the very
`then_fast()` calls that same event's own `set()` needed to see coming
first. The rewritten "resolves synchronously when every future is
already ready" test (previously "resolves immediately", tolerant of a
`run_until_idle()` round trip) caught this immediately: `combined.ready()`
came back false right after `when_all()` returned, even with both inputs
already resolved. Fixed by reordering `when_all()`/`when_all(std::span<...>)`
themselves: build `when_all_state`, register every `when_all_track()`
call, *then* call `state->event.wait()` last - by which point `set()`
has already fired if every input turned out to be ready, letting
`wait()`'s own fast path resolve the whole call synchronously end to
end. `detail::when_all_setup()` shrank to just the "build the state, or
an empty one for `count == 0`" half of what it used to do, since the
future-returning half can no longer safely live there.

Wiki (`docs/wiki/Continuation-Node-Mechanism.md`, `docs/wiki/Architecture.md`)
and the module's own doc comments (`est/src/when_all.cppm`) rewritten to
match: `then()` → `then_fast()` throughout, the `one_shot_event`-based
state, the new `:sync.event` dependency edge, and a dedicated writeup of
why `event.wait()` has to be called last.

**Re-verified in the pinned Docker devenv after the follow-up:**
194/194 tests pass; `clang-format`/`clang-tidy` clean; 161/161 tests pass
under the `sanitize` preset too; `diff-cover` coverage gate against
`main` at 98% (170/173 changed lines covered - the same 3 boilerplate
lines as before).

---

### `shared_ptr<T>`'s two specializations deduplicated via a CRTP base (done)

A framework-wide sweep for repeated logic turned up one clear candidate:
`est::shared_ptr<T>`'s primary template (a `T` boxed inside a heap-allocated
control block) and its `est::ref_counted`-based partial specialization
(`T` carries its own ref count and allocator directly, no wrapping struct)
duplicated the same ~90 lines of copy/move/`swap()`/`reset()`/`get()`/
`operator*`/`operator->`/`operator bool`/`count()` almost verbatim - the
only real difference between the two is *how* to reach the ref count and
the pointee from the one thing each specialization actually stores
differently (a `control_block*` vs. a `T*`), and what "destroy" means for
each.

Factored that shared machinery into one CRTP base,
`detail::shared_ptr_common<Derived, Pointer, T>` (`est/src/util/shared_ptr.cppm`),
storing the single `Pointer` and implementing every one of those members in
terms of three static accessors it calls on `Derived`: `element_of(ptr)`,
`ref_count_of(ptr)`, `destroy(ptr)`. Each `shared_ptr<T>` specialization now
only defines `make()` (which differs in allocation shape - one allocation
combining ref count + allocator + `T` for the primary template, vs. `T`
allocated directly for the `ref_counted` case) and those three tiny
accessors, then inherits the base for everything else; copy/move/the
destructor are left fully implicit on both specializations (neither adds
a member of its own beyond `base`), so they just forward straight to
`shared_ptr_common`'s own. `shared_ptr_control_block<T>` (the primary
template's boxed-`T` struct: allocator + ref count + `T`) moved out of
being a private nested type of `shared_ptr<T>` into a free struct in
`namespace est::detail`, since a class's base-specifier list (needed here,
for `Pointer = shared_ptr_control_block<T>*`) is evaluated before the
class body opens and can't name a type nested inside that same class.

**`bugprone-crtp-constructor-accessibility`, caught by `clang-tidy`:**
the base's first cut left its default/copy/move/pointer-adopting
constructors implicitly public - which `clang-tidy` correctly flagged as
letting the mixin be constructed or inherited from outside its one
intended pairing with `shared_ptr<T>`. Fixed the same way this codebase's
own pre-existing `current_allocator_new_delete<Derived>` mixin (see the
entry above) already settled the identical shape: every constructor moved
`private`, plus `friend Derived;` so only the one intended derived
specialization can reach them.

**A real `std::uses_allocator` regression, caught by the build, not by
`shared_ptr_tests.cpp`:** `shared_ptr_control_block<T>`'s first draft
carried its own `using allocator_type = std::pmr::polymorphic_allocator<std::byte>;`
member - copied out of habit from the class it replaced, but a member the
*original* nested `control_block` never actually had (it only ever saw
`allocator_type` via its enclosing class's scope). That one extra typedef
is exactly what `std::uses_allocator` construction keys off of:
`polymorphic_allocator::new_object<T>()`'s internal `construct()` checks
whether `T` itself declares a convertible `allocator_type` and, if so,
silently appends a *second*, trailing allocator argument on top of
whatever was already passed explicitly. `shared_ptr_control_block<T>`'s
constructor already takes the allocator as its first parameter by design
(it's stored, then read back by `destroy()`), so the extra trailing one
broke every call site with more than a bare no-arg `T` - caught only when
`est/src/when_all.cppm` (`detail::when_all_state`, merged separately while
this refactor was in progress) failed to compile with a constructor-
overload-resolution error, not by any existing test. Fixed by removing the
stray `allocator_type` member and spelling out
`std::pmr::polymorphic_allocator<std::byte>` directly in the constructor
parameter and the stored member's type instead.

Three weaker duplication candidates surfaced by the same sweep were left
alone as not worth the abstraction: `detail::ready_node`/`timer_node`'s
parallel `abandon_*`/`destroy()` shape (already intentionally distinct
naming, not accidental duplication); `future_state<T>::set_value(const
U&)`/`set_value(U&&)` (an idiomatic copy/move pair, not repetition); and
`sleep_until()`/`yield_execution()`'s shared "build a node, register it,
return its future" shape (too few call sites to earn a shared helper).

Docs updated: `docs/wiki/Home.md`'s source-location table (added
`detail::shared_ptr_common`/`detail::shared_ptr_control_block<T>` next to
`est::shared_ptr<T>`/`est::ref_counted`). `Architecture.md` and
`Allocation-Patterns.md` needed no changes - both already describe
`shared_ptr<T>`'s externally observable allocation behavior, which this
refactor doesn't change, rather than its old internal nested-`control_block`
layout.

**Verified in the pinned Docker devenv:** 194/194 tests pass;
`clang-format`/`clang-tidy` clean (zero user-code findings); full suite
passes under the `sanitize` preset (ASan+UBSan) too, at 161/161; `diff-cover`
coverage gate against `main` at 98.1% (51/52 changed lines covered - the
one miss is the primary template's own class-declaration line, not
executable code).

---

### Issue #54, part two: `est::when_any()`

The follow-up `est::when_all()` itself deferred: same ownership
contract (each `future<T>&` stays the caller's own; `when_any()` never
consumes, moves, or reads a value/exception out of any input itself),
and built the same way from the start this time - `then_fast()` at both
tracking stages, a `one_shot_event<EventResetMode::manual>` for the
completion signal, and `event.wait()` called last, after every input is
registered - all three of which `est::when_all()` only reached after a
PR review round and a bug of its own; see that entry, above, and
`docs/wiki/Continuation-Node-Mechanism.md`'s "`est::when_any()`: the
same shape, with no counter at all" for why each one still applies here
unchanged. `est::when_any()` resolves the moment *any one* of its inputs
is accounted for - completed or abandoned, succeeded or failed - and
never fails or cancels the inputs that didn't win, which simply keep
running to completion in the background (this codebase has no
cancellation mechanism at all, the same accepted constraint
`est::when_all()`'s own doc comment already states).

**Simpler than `when_all` in one real way: no counter.** `when_all_state`
needs a `remaining` count because *every* input has to be accounted for
before its event can fire. `when_any_track()` shares nothing but the
`one_shot_event` itself and calls `set()` unconditionally - safe
unmodified, since `one_shot_event<Mode>::set()` already tolerates being
called redundantly by as many races as reach it (its own doc comment
calls out exactly this shape: independent cancellation sources racing to
fire the same one-shot signal, with no coordination required). No
`when_any_state` wrapper struct exists at all - `shared_ptr<one_shot_event
<EventResetMode::manual>>` is the entire shared state.

**Empty case handled the opposite way from `when_all`.** "Any one of
zero" has nothing that could ever complete it - not vacuously true the
way `when_all`'s own empty case is - so the fixed-arity overload
`static_assert`s `sizeof...(Ts) > 0` at compile time (a pack's size is
always known then), and the `std::span<future<T>>` overload `check()`s
the same precondition at runtime instead, since a span's size isn't
visible to the compiler. Matches this codebase's own established
"no practical way to unit-test a `check()` failure without process-
isolation tooling" precedent (`est/tests/check_tests.cpp`) - not
exercised by a test, same as every other `check()` call site in this
codebase.

**Docs:** `docs/wiki/Home.md`'s source-location table; a new `:when_any`
node and edges in `docs/wiki/Architecture.md`'s dependency graph
(notably no edge to `:promise` - unlike `when_all`, `when_any` has no
empty-case `make_ready_future()` call, so it never needs `:promise` at
all); the full mechanism written up in
`docs/wiki/Continuation-Node-Mechanism.md`, alongside `est::when_all()`'s
own entry.

**Tests added** (`est/tests/when_any_tests.cpp`): resolves once any one
input is ready, not before; resolves synchronously when at least one
input is already ready at call time (guards the identical `event.wait()`
-ordering correctness `when_all()` needed, verified correct from the
start here rather than caught after the fact); resolves on a failed
input the same as a succeeded one; resolves when one input is abandoned
while another stays genuinely pending (mirrors `when_all()`'s own
abandonment regression test); does not consume the caller's futures;
works across heterogeneous types including `future<void>`; the
`std::span` overload's own any-one-ready case; two leak tests (freed on
the happy path - including the redundant `set()` from whichever input
loses - and freed even when the winning input is abandoned rather than
completed).

**Verified in the pinned Docker devenv:** 203/203 tests pass (9 new);
`clang-format`/`clang-tidy` clean; 170/170 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 97% (140/143 changed lines covered - the same
`counting_resource::do_is_equal()` boilerplate pattern as every other
test file that defines one).

---

### `est::when_any_succeeds()`: a race with a value-carrying result

Requested directly: "a `when_any_succeeds` that waits for a
non-exception future" among its inputs, "returns `future<bool>`, false
when none finished successfully" - with the same ownership contract
(`future<T>&`, caller-owned) and the same overall shape (two overloads,
a two-stage `then_fast()` chain per input) `est::when_all()` had already
settled on. `est::when_any_succeeds(future<Ts>&... futures) -> future<bool>`
(`est/src/when_any_succeeds.cppm`) resolves `true` the moment *any one*
input succeeds, or `false` once *every one* has failed (or been
abandoned) without any succeeding - a race for the first success, with a
"nobody won" fallback that can only fire once nothing is left that could
still win.

**The tracking chain's first stage isn't a no-op here, unlike
`when_all_track()`'s own.** `when_any_succeeds()` needs to know *which
way* each input finished, not just *that* it did, so
`when_any_succeeds_track()`'s first `then_fast()` stage rethrows the
input's own stored exception when it failed - reusing
`concrete_continuation<Fn, U>::run()`'s existing callback-exception
routing (est:future) to translate "input failed" into "this stage's own
`future_state` fails, with the same exception," with no new mechanism
needed. Input succeeding, or being abandoned instead of completed
(`abandon()` always fails its own downstream regardless of whether the
callback ever ran), both land on the second stage's `completed.failed()`
check without it ever needing to tell the two apart - abandonment isn't
a success, and that's the only distinction that matters from here on.

**A `done` flag, not just a counter, guards the result.** Two different
paths can complete `result`: the first success (immediately, however
early), or the last unaccounted-for failure once `remaining` reaches
zero - and only one of those two may ever actually call `set_value()`,
on pain of a checked precondition violation (completing an
already-completed `future_state` twice). Every hook checks `done` first
and does nothing if it's already set; single-threaded and loop-driven,
this needs no atomics, since hooks never interleave with each other.
Missed on the first pass and caught by `diff-cover`, not a test failure:
the `done`-is-already-set early return had no test exercising it at all
until "ignores a later completion once it has already resolved" was
added specifically to cover it - not just for the coverage number, but
because an untested guard against a real crash (a second `set_value()`
call) is exactly the kind of code most worth a dedicated test.

**No event, unlike `when_all`/`when_any` - and no ordering subtlety to
get wrong as a result.** `est::one_shot_event` can only ever signal that
*something* happened, never carry a value - so a `bool`-valued result
needs a plain `promise<bool>`/`future<bool>` pair (`make_promise_future
<bool>()`, built once, up front) instead. That sidesteps `when_all()`'s
own "`event.wait()` must be called last" hazard entirely (see its own
entry, above): a plain `future`'s readiness is checked fresh against its
`future_state` every time, with no "already-signaled fast path" of its
own to accidentally register a waiter against too early - unlike
`one_shot_event::wait()`, timing relative to registering inputs simply
doesn't matter here.

**Empty case resolves `false`, the opposite of `when_all`'s vacuous
`true`, for the same underlying reason.** "Does at least one of these
succeed" is false over an empty set - there's nothing that could have
succeeded - the mirror image of "has everything completed" being
vacuously true over the same empty set.

**Docs:** `docs/wiki/Home.md`'s source-location table; a new
`:when_any_succeeds` node and edges in `docs/wiki/Architecture.md`'s
dependency graph (no `:sync.event` edge, unlike `:when_all` - explained
above); the full mechanism written up in
`docs/wiki/Continuation-Node-Mechanism.md`, alongside `est::when_all()`'s
own entry.

**Tests added** (`est/tests/when_any_succeeds_tests.cpp`): resolves true
as soon as one input succeeds; does not resolve while one input has
failed but another is still pending; resolves false only once every
input has failed; resolves true even when it wins after some inputs
already failed; resolves synchronously when one input already succeeded
at call time; an empty pack resolves false immediately; ignores a later
completion once already resolved (the `done`-flag test above); treats an
abandoned input the same as a failed one, not a success; does not
consume the caller's futures; the `std::span` overload's own any-one-
succeeds case and empty-range case; two leak tests (freed on the happy
path - including a redundant completion from whichever input loses -
and freed even when the winning input is abandoned rather than
completed).

**Verified in the pinned Docker devenv:** 207/207 tests pass (11 new);
`clang-format`/`clang-tidy` clean; 174/174 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 98% (196/199 changed lines covered - the same
`counting_resource::do_is_equal()` boilerplate pattern as every other
test file that defines one).

---

### Issue #73: `est::schedule_periodic()` - a periodic timer, plus `est::jitter`

Requested directly, with two concrete pieces named up front: "build #73,
create a util jitter that performs uniform distribution jitter. Add on
platform a `get_random_seed()` method." `est::timer_queue`/`est::loop`
had no repeating-timer concept at all going in - `loop::fire_ready_timers()`
unconditionally deletes every `timer_node` right after `fire()` returns,
the same one-shot contract every existing node type in this codebase
already relies on (`sleep_resume_node`, `promise_resume_node<T>`).

**`platform::interface::get_random_seed() -> std::uint64_t`** (new pure
virtual, `est/src/platform/platform.cppm`) - "where does randomness come
from" answered the same way `now()`/`assert_failure()` already are: a
platform decision, not something `est` itself sources directly.
`hosted_stdcpp` (`estext`) answers via `std::random_device`, falling back
to `now()`'s own bit pattern if that throws (mirroring the same
try/catch-and-fall-back shape its own `assert_failure()`/`vprintdbg()`
already use for their own fallible `std::` calls). Every existing
`platform::interface` implementation - the four test fakes
(`platform_tests.cpp`, `timer_tests.cpp`, `loop_tests.cpp`'s two) plus
`hosted_stdcpp` itself - needed a new override; the fakes all return a
fixed constant, keeping any test that indirectly exercises `est::jitter`
reproducible rather than flaky. `platform_tests.cpp` gained a fifth
dispatch test (`get_random_seed()` retargets through
`override_instance()`, matching its existing `sleep_until()`/`printdbg()`
tests' own shape).

**`est::jitter`** (`est/src/util/jitter.cppm`, `:util.jitter`) - a small,
self-seeding uniform-jitter generator: `std::minstd_rand` (a single-word-
state 32-bit Lehmer/Park-Miller LCG, not `std::mt19937`'s much larger
state - plenty of quality for spreading out wakeups, no reason for this
codebase's eventual bare-metal target to carry more) plus a
`std::uniform_int_distribution` over `[-max_jitter, +max_jitter]`. Seeds
itself once, at construction, from `platform::instance().get_random_seed()`
- the one place in this codebase that needs actual randomness. Not
cryptographically secure, nor does it need to be: jitter only has to
differ from the last draw, never resist prediction. `max_jitter` must be
non-negative (`est::check()`-enforced, matching `counting_event`'s own
`max_count > 0` precondition shape - checked after being used to build
the distribution's bounds, via a small `clamp()` helper, so a negative
value can't itself feed `std::uniform_int_distribution` invalid bounds
before the check has a chance to fire).

**`est::schedule_periodic(interval, fn, max_jitter = {})`**
(`est/src/timer_periodic.cppm`, `:timer.periodic`) - built entirely as
sugar on top of `loop::schedule_timer()`, the same way `sleep_for()`/
`yield_execution()` are (`est:promise`), with zero changes to `loop.cppm`
itself. Since a `timer_node` can't survive its own firing,
`detail::periodic_timer_node<Fn>::fire()` hands off to a **fresh** node
for the next period before returning, moving `fn_`/`jitter_` forward into
it so the same callable and the same PRNG state (not a freshly-reseeded
one) carry across the whole chain - only the node's own allocation is new
each period, consistent with every other node type in this codebase being
a one-shot, freshly-allocated object. Cancellation is one small shared
`detail::periodic_timer_control{bool cancelled}` (`est::shared_ptr`,
referenced by every node in the chain and by the `periodic_timer_handle`
returned to the caller) - plain, not atomic, since this is entirely
loop-thread-side state, the same single-threaded assumption as everywhere
else in `est`. `fire()` checks it twice: once before calling `fn_()` (so
an already-scheduled-but-not-yet-fired node stops cleanly once
cancelled), once after (so `fn_` cancelling itself, mid-call, actually
prevents that same call from rescheduling one more time).

**`interval` must be positive and `max_jitter` strictly less than
`interval`, both checked** - not merely `<=`: a jittered delay of exactly
zero risks the rescheduled node landing in the very timer batch that's
still firing (`loop::fire_ready_timers()` evaluates "now" once per batch,
so a same-instant reschedule would be picked up and re-fired before that
batch ever returns control to `loop`'s own outer loop) - jitter perturbs
a period, it doesn't get to invert or collapse one.

**`std::move()` removed on every `est::jitter` handoff, per `clang-tidy`:**
the first draft moved `jitter_`/`jit` into each successive node
(mirroring how `fn_`/`ctrl_` are actually moved) - `performance-move-
const-arg` correctly flagged this as pointless: `est::jitter` is
trivially copyable (two small integers' worth of state), so a move costs
exactly what a copy does. Fixed by passing it by value/copy at every call
site instead, with a comment explaining why - this is the one place in
the node's construction that *isn't* a move, deliberately.

**Docs:** `docs/wiki/Home.md`'s source-location table (`est::jitter`,
`est::schedule_periodic()`/`periodic_timer_handle`/`detail::
periodic_timer_node<Fn>`, and `get_random_seed()` added to the platform
seam's own row); a new `:util.jitter`/`:timer.periodic` pair of nodes and
edges in `docs/wiki/Architecture.md`'s dependency graph, plus a paragraph
on why `:timer.periodic` depends on `:util.current_loop`/`:util.jitter`/
`:util.shared_ptr` but deliberately not `:future`/`:promise` (nothing
about `schedule_periodic()` is awaited); a full new "Periodic timers"
section in `docs/wiki/Loop-And-Timers.md`, placed right after the
existing `schedule_timer()`/`future<void>` bridge section it builds on.

**Tests added:** `est/tests/jitter_tests.cpp` (draws stay within
`[-max_jitter, +max_jitter]`; a zero `max_jitter` always draws zero; many
draws produce more than a couple of distinct values - not a fixed value
or a two-value alternation). `est/tests/timer_periodic_tests.cpp`, driven
by the same fake-clock `platform::interface` pattern `loop_tests.cpp`
established: fires once per period until self-cancelled from inside the
callback; each jittered delay stays within `[interval - max_jitter,
interval + max_jitter]`; `cancel()` called from outside stops the chain
before its next scheduled firing (via `loop.stop()` to break out of the
first `run_until_idle()`, since a periodic chain that never self-cancels
would otherwise never go idle on its own); the node chain and control
block are freed, not leaked, across several periods; a still-pending node
is abandoned, not leaked, when the loop is destroyed before ever firing.

**Verified in the pinned Docker devenv:** 225/225 tests pass (18 new);
`clang-format`/`clang-tidy` clean (the `performance-move-const-arg`
finding above, fixed before this line); 192/192 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 91% (181/199 changed lines covered - both new source files at
100%; the misses are unused fake-platform boilerplate overrides in test
files that don't exercise `est::jitter`, an unreachable
`std::random_device`-throws fallback branch in `hosted_stdcpp` matching
this file's own existing unreachable-catch pattern, and a couple of
`REQUIRE(...)` line-attribution artifacts in the new test files
themselves).

#### Follow-up code review: two real findings, both fixed

A `/code-review` pass over this PR's diff (single-pass, no subagent
fan-out available) found two genuine issues, both independently verified
against the actual code before fixing:

**`periodic_timer_node::fire()` had no exception handling around `fn_()`,
unlike every other user-callback path in this codebase** -
`concrete_continuation<Fn, U>::run()` (`est:future`) wraps its own `fn_(...)`
call in `try`/`catch (...)`, routing any exception into
`downstream_->set_exception()`. `fire()` had nothing comparable: a
throwing `schedule_periodic()` callback would propagate straight out of
`loop::fire_ready_timers()`/`run_impl()`, unwinding the *entire* loop over
one periodic callback's own bug - abandoning every other unrelated
pending timer and ready-work item mid-drain, and silently killing the
periodic chain forever with no diagnostic. No test exercised a throwing
callback. Fixed by wrapping `fn_()` in `try`/`catch (...)`, reporting via
`platform::printdbg()` (the same "loud diagnostic, don't stop the loop"
tool `loop::run_one()`'s own long-running-callback stall detection
already uses) and letting the chain still reschedule - `fn_` returns
`void`, not a `future<T>`, so there's no downstream to route the
exception into the way `.then()` continuations do; treating one bad
period as skipped rather than fatal is the closer match to what
"periodic" implies. New test: `fn()` throwing on one period is skipped,
doesn't escape `run_until_idle()`, and the chain still reaches (and
honors) a later cancelling call.

**`est::jitter` discarded half the entropy `get_random_seed()` computed,
for nothing.** `hosted_stdcpp::get_random_seed()` combines *two*
`std::random_device` draws into its 64-bit return value
(`(dev() << 32) | dev()`), but `jitter`'s constructor fed that straight
into `static_cast<std::minstd_rand::result_type>(seed)` - a 32-bit type,
so the cast silently truncated to the low 32 bits, meaning the *entire
first* `random_device` draw was computed and then thrown away unused.
Not a crash, but a real waste (a `std::random_device` draw can be slow -
some implementations gather real hardware entropy per call - and a
future bare-metal backend's entropy source may be scarcer still) and a
broken contract (a 64-bit seed getter whose only caller only ever
benefited from half of it). Fixed with an XOR-fold (`seed ^ (seed >> 32)`)
before the narrowing cast, in `jitter`'s own constructor - both halves
now contribute, and `get_random_seed()`'s 64-bit contract stays honest
for any future caller that might want more than 32 bits directly, with
no change needed on the `hosted_stdcpp` side at all.

**Re-verified in the pinned Docker devenv after both fixes:** 226/226
tests pass (1 new); `clang-format`/`clang-tidy` clean; 193/193 tests pass
under the `sanitize` preset too; `diff-cover` coverage gate against `main`
at 93% (both fixed source files still at 100%).

#### Follow-up: fixed-rate scheduling, and `Fn` constrained to `std::invocable<Fn&>`

Requested directly, two changes: "Periodic timer should measure time
before calling the function. Fn should be std::invocable."

**Fixed-delay, not fixed-rate - a real drift bug.**
`periodic_timer_node::fire()`'s first cut computed the next deadline from
`platform::instance().now()` called *after* `fn_()` returned, not before
- so a slow or variable-latency `fn_()` would push every later period
further out by however long that call took, compounding period over
period (classic fixed-delay scheduling, like a naive `setTimeout()`
chain, not the fixed-rate `setInterval()`-style cadence a "periodic
timer" implies). Fixed by capturing `platform::instance().now()` into a
local (`period_start`) at the very top of `fire()`, before `fn_()` runs,
and scheduling the next node at `period_start + interval_ + offset`
instead of a post-call `now()` read. The very first period
(`schedule_periodic()` itself) was already correct by construction -
nothing has called `fn()` yet at that point - so only `fire()`'s own
rescheduling needed the fix. New test
(`est/tests/timer_periodic_tests.cpp`): `fn()` itself advances the fake
clock by 400ms (simulating a slow callback) each call, interval 1s -
asserts consecutive call *start* times are exactly `interval` apart, not
`interval + 400ms`; this test fails under the old code and passes under
the fix, making it a genuine regression guard rather than a
restatement of the implementation.

**`Fn` constrained to `std::invocable<Fn&>`**, on both
`detail::periodic_timer_node<Fn>` and `schedule_periodic()` themselves -
matching `est::scope_exit`'s own established pattern
(`est:util.scope_exit`) of constraining a stored-and-later-invoked `Fn`
at the type, not only at whatever function happens to construct it, so a
caller passing something non-callable gets a constraint-failure
diagnostic pointing at the actual mismatch instead of a template-
instantiation error buried inside `fire()`'s body. `std::invocable<Fn&>`,
not plain `std::invocable<Fn>`: `fn_` is invoked repeatedly, as a named
(non-const lvalue) member, once per period - the same distinction
`future.cppm`'s own `invocable_unwrapped<Fn, T>()`/`then_callback_for<Fn,
T>` already draw between a callable invoked once (`Fn`, `scope_exit`'s own
shape) and one stored and invoked more than once (`Fn&`).

**Re-verified in the pinned Docker devenv:** 227/227 tests pass (1 new);
`clang-format`/`clang-tidy` clean (one `cppcoreguidelines-pro-bounds-
avoid-unchecked-container-access` finding on the new test's own
`std::vector::operator[]` calls, fixed by switching to `.at()` - matching
`when_all_tests.cpp`/`when_any_tests.cpp`/`when_any_succeeds_tests.cpp`'s
own existing convention); 194/194 tests pass under the `sanitize` preset
too; `diff-cover` coverage gate against `main` at 93%
(`timer_periodic.cppm` still 100%).

---

### Issue #74: `est::external_event<T>` - bridging an externally-written value

The other half of issue #73's own periodic-timer work, as specced out in
[#74's own planning comment](https://github.com/andijcr/async_experiments/issues/74#issuecomment-5652953980)
before #73 landed: "Implement `external_event::set/poll`, use periodic
timer to poll... bridging external events with events." `est::
external_event<T>` (`est/src/sync/external_event.cppm`,
`:sync.external_event`) wraps a caller-owned `std::atomic<T>&` - written
from another thread, an ISR, or (eventually) a hardware register - and
bridges it into the existing `est::binary_event<EventResetMode::manual>`
(`est:sync.event`) machinery: `poll()` (loop-thread only, never suspends)
is the *one* place this class ever reads the atomic, and the *only*
genuinely cross-thread state anywhere in this codebase - the deliberate
exception to `est`'s single-threaded/no-atomics rule (`CLAUDE.md`), not
an accidental one.

**Shape settled largely as spec'd**, with one naming change: the spec's
own `acknowledge()` became plain `reset()` instead, matching every other
`EventResetMode::manual` primitive's own naming
(`counting_event<manual>::reset()`, `binary_event<manual>::reset()`)
rather than inventing a new name for the identical "clear the signal,
ready for the next one" operation. Level-triggered, not edge-triggered:
`poll()` calls `event_.set()` (idempotent, stays signaled until
`reset()`) rather than trying to fire once per distinct value, so a
`wait()` called after several changes already happened still resolves
immediately via `binary_event`'s own already-signaled fast path,
observing the *latest* value through `value()` - a change before
`reset()` folds into whatever `reset()` next observes rather than being
lost or double-counted, and a change after `reset()` sets the event
again.

**`T` constrained twice**: `std::equality_comparable<T>` (a `requires`
clause - `poll()` needs `!=` to detect a change) and
`std::atomic<T>::is_always_lock_free` (a `static_assert` - `poll()` is
meant to be callable from a context as constrained as a periodic timer
callback, eventually an ISR, so it must never silently block on a
fallback lock the way a non-lock-free `std::atomic<T>` could).

**Deliberately not started/owned by this class** - a caller wires
`poll()` into `est::schedule_periodic()` (issue #73, above) explicitly,
rather than `external_event<T>` starting a periodic timer of its own:
lets one periodic timer's callback drive several sources' `poll()` calls
at once, or drive `poll()` from something other than a timer entirely,
and keeps this class trivially unit-testable without any real second
thread - a test just assigns directly to the `std::atomic<T>` it
constructs the bridge over, single-threaded, matching every other test
in this codebase.

**Two `clang-tidy` findings, both fixed by a cleaner design rather than a
suppression:**
- `cppcoreguidelines-avoid-const-or-ref-data-members` on the first
  draft's `std::atomic<T>& source_` member. Fixed by storing
  `std::atomic<T>*` instead - `event_` (a `binary_event<Mode>`) already
  makes this class non-copyable/non-movable regardless, inheriting
  `counting_event<Mode>`'s own deleted special members, so the pointer
  costs nothing functionally and sidesteps the guideline cleanly (the
  constructor still takes `std::atomic<T>&`, only the stored
  representation changed).
- `cppcoreguidelines-avoid-capturing-lambda-coroutines` on the new
  integration test's own `[&]() -> future<void> { ... }()` - a genuine
  use-after-free hazard, not a false positive: an immediately-invoked
  capturing lambda coroutine's closure object (holding the captures) is
  a temporary that dies at the end of the full-expression, while the
  coroutine frame it produced can outlive it. Fixed by switching to a
  captureless lambda taking its dependencies as parameters instead
  (copied straight into the coroutine frame, not the closure) - the same
  pattern `est/tests/event_tests.cpp`'s own `waiter` lambda already
  established, `NOLINTBEGIN`/`NOLINTEND`-wrapped for the companion
  `cppcoreguidelines-avoid-reference-coroutine-parameters` finding that
  shape itself triggers.

**Docs:** `docs/wiki/Home.md`'s source-location table; a new
`:sync.external_event` node and edges in `docs/wiki/Architecture.md`'s
dependency graph, plus a paragraph on why it depends on `:sync.event`/
`:future` but not `:platform` (unlike `:util.jitter`, it never calls
`platform::instance()` itself); a full new "Bridging external writers"
section in `docs/wiki/Loop-And-Timers.md`, placed right after `est::
jitter`'s own section.

**Tests added** (`est/tests/external_event_tests.cpp`): `poll()` with no
change is a no-op; `poll()` after a change resolves a queued `wait()`;
`wait()` called after an already-observed change resolves synchronously
(the `binary_event` fast path); multiple concurrent waiters all resolve
off one `poll()`; a second change before `reset()` is folded into
`value()` without double-firing; `reset()` re-arms for the next change;
both `std::atomic<bool>` (flag-style) and `std::atomic<int>` (value-style)
as `T`; and a full integration test bridging into a real
`schedule_periodic()` poll loop end to end, matching the issue's own
motivating shape.

**Verified in the pinned Docker devenv:** 235/235 tests pass (8 new);
`clang-format`/`clang-tidy` clean (both findings above, fixed before this
line); 202/202 tests pass under the `sanitize` preset (ASan+UBSan) too;
`diff-cover` coverage gate against `main` at 98% (`external_event.cppm`
itself at 100%; the one miss is an unexercised fake-platform
`assert_failure()` override in the new integration test, the same
boilerplate-never-called pattern every other fake platform in this
codebase's test suite already has).

---

### Issue #47's pooling alternative, measured: `std::pmr::unsynchronized_pool_resource`

Follow-up to issue #47's own alternative (3) - "a dedicated small-object
pool for `future_resume_node<T>`... doesn't remove the allocation count,
only its cost." Rather than write one, tried the cheapest possible version
first: since every allocation in this codebase already flows through the
loop's own `std::pmr::polymorphic_allocator<std::byte>`
(`docs/wiki/Allocation-Patterns.md`), a caller can swap in
`std::pmr::unsynchronized_pool_resource` (the single-threaded variant,
matching `est::loop`'s own no-atomics constraint) as the backing resource
with zero framework changes at all - `est::loop loop{&pool};` instead of
`est::loop loop;`.

**Measured, not just argued.** A throwaway benchmark (`examples/probe/`,
temporarily rewritten, not committed) drove 200,000 iterations of a mixed
workload - one genuinely-suspending `co_await` plus one `.then()` chain
per iteration, 1.4 million allocate/deallocate pairs total - under a
Release+LTO build, timing the same workload against `new_delete_resource()`
directly versus `unsynchronized_pool_resource` wrapping it. Result: ~50-52ms
pooled versus ~56-58ms unpooled, a consistent ~9-10% improvement, confirmed
with the run order swapped (pooled-then-baseline as well as
baseline-then-pooled) to rule out warm-up bias rather than a genuine
resource-level effect. Allocation *counts* were identical in every run,
confirming the win is purely `malloc`/`free` cost, not a change in how
many allocations happen.

**Conclusion matches issue #47's own recommendation.** A real, repeatable
win, but a modest one - this codebase's own allocations are already cheap
enough that a generic pool resource captures most of the available gain
without writing a bespoke freelist sized to one specific node type (issue
#47's alternative (2), the invasive embedded-resume-node redesign, remains
unnecessary on this evidence).

**Landed:** `docs/wiki/Allocation-Patterns.md` gained a "Pooling: measured,
not just theoretical" section with the full numbers and the opt-in
snippet; `examples/hello_world/main.cpp` now constructs its `est::loop`
against a `std::pmr::unsynchronized_pool_resource` as a worked example of
the opt-in pattern (not because `hello_world` itself needs it - it's the
first place a reader looks for "how do I set this up"). A comment
recording this finding was also added to issue #47 itself.

**Re-verified in the pinned Docker devenv:** `clang-format`/`clang-tidy`
clean; `ci` preset build + 227/227 tests pass (no test-visible behavior
changed - `hello_world` isn't part of the test suite, run manually to
confirm it still prints `est::future value: 42`).

---

### Issue #96: `est::spsc_ring<T>` - a lock-free bounded queue, composing with `external_event<T>`/`schedule_periodic()`

Requested as the natural follow-up to issue #74's own `external_event<T>`:
"a fixed size circular queue (at runtime) written by the external context
and read in the loop context, no mutex." Spec'd out first as its own
issue (#96) before any code, settling the open questions the issue itself
raised, then implemented largely as spec'd.

**Why this isn't `external_event<T>` generalized.** `external_event<T>`
is level-triggered on a single value - writes between two `poll()` calls
collapse into one, the right tradeoff for a sensor reading, wrong for a
stream where every value matters (log records, incoming frames, queued
commands). `est::spsc_ring<T>` (`est/src/sync/spsc_ring.cppm`,
`:sync.spsc_ring`) delivers every successfully pushed value exactly once,
in order, or rejects it outright while full - never silently drops one.
The two solve different problems and neither is built in terms of the
other; see the "composition, not reuse" section of the design comment
below.

**Open questions from #96, settled:**
- **Full/empty disambiguation** → `capacity() + 1` slots, the classic
  trick: full is `advance(write_index) == read_index`, empty is
  `write_index == read_index`, needing no separate atomic count or
  generation bit alongside the two indices already required.
- **Overwrite-on-full vs. reject-on-full** → reject (`try_push()` returns
  `false`, leaves the value unconsumed) - the safer default for the
  motivating "this data actually matters" examples; an "overwrite oldest"
  policy for a metrics/telemetry use case is left as a real but
  unimplemented future option, not guessed at.
- **`T`'s constraints** → started as `std::is_trivially_copyable_v<T> &&
  std::default_initializable<T>`, then relaxed before merge (see below) to
  `std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>
  && std::default_initializable<T>` - a strict superset (a trivially
  copyable type's "move" is the same non-throwing copy it always was),
  letting a move-only slot type work too, most usefully
  `est::spsc_ring<std::unique_ptr<U>>`.
- **Where it lives** → its own partition, `est/src/sync/spsc_ring.cppm`
  (`:sync.spsc_ring`), alongside `mutex.cppm`/`event.cppm`/
  `external_event.cppm` rather than folded into any of them - genuinely
  standalone in the dependency graph (depends only on `:check`, not even
  `:platform`: nothing in its own algorithm needs a clock or a random
  seed).
- **Memory ordering** → given its own full writeup in
  `docs/wiki/Loop-And-Timers.md`'s new "A stream instead of a value"
  section, as the issue itself asked for: two atomics (`write_index_`,
  `read_index_`), each written by exactly one side, each side's own
  `relaxed` self-read paired against the other side's `acquire`/`release`
  cross-read - `try_push()`'s `write_index_.store(release)` is what
  actually publishes the pushed value (everything sequenced before it,
  including the plain unsynchronized write into `buffer_`), paired with
  `try_pop()`'s own `write_index_.load(acquire)`; the reverse pairing on
  `read_index_` publishes a freed slot back to the producer the same way.
- **Testing** → the same single-threaded-simulation approach
  `external_event_tests.cpp` established generalizes cleanly: `try_push()`
  called directly from the test body, no real second thread needed to
  exercise the logic correctly - plus one test that does use a real
  `std::jthread` producer racing a `schedule_periodic()`-driven consumer
  over wall-clock time (see below), since `spsc_ring<T>` is the one type
  in this codebase whose whole contract is an actual cross-thread handoff.

**A real tension between two `clang-tidy` checks, resolved by keeping the
unchecked access.** The first draft used `buffer_.at(index)` (bounds-
checked) to satisfy `cppcoreguidelines-pro-bounds-avoid-unchecked-
container-access` - which immediately tripped `bugprone-exception-escape`
instead, since `.at()` can throw and both `try_push()`/`try_pop()` are
(deliberately - see the design comment) `noexcept`. Unlike the earlier
`current_allocator_new_delete<Derived>` case where two checks wanted
contradictory things for an equally-valid shape, this one has an actual
answer: `write_index`/`read_index` are provably always `< buffer_.size()`
by construction (`advance()` itself never returns otherwise), so a
bounds check can only ever pass silently or - if the invariant were ever
violated by a bug - throw straight through a `noexcept` function into
`std::terminate()`, strictly worse than the plain access, not safer, for
a case that cannot occur. Reverted to `buffer_[index]`, each site keeping
a `NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)`
plus a comment stating the invariant, rather than silently suppressing
without explanation.

**Relaxed `T` to nothrow-movable before merge, and a real bug that fell
out of it.** Requested during review: support a move-only slot type (the
motivating case, `est::spsc_ring<std::unique_ptr<U>>`) rather than only
trivially copyable ones. The constraint relaxation itself was mechanical
(`std::move()` into/out of `buffer_` instead of copy-assignment) - but
`try_push(T value)`'s original by-value parameter turned out to be a real
correctness bug for a move-only `T`, not just a style question. A
by-value parameter moves out of the caller's argument at the *call site*,
unconditionally, before `try_push()`'s own body ever checks whether the
ring is full - so `try_push(std::move(rejected))` against a full ring
would already have destroyed `rejected`'s contents with no way to hand
them back, silently violating this class's own stated "never silently
drops" guarantee the moment `T` stopped being copyable (copying doesn't
have this hazard: the discarded parameter is a copy, the caller's
original is untouched either way, which is exactly why it went unnoticed
under the original trivially-copyable-only constraint). Fixed by changing
the parameter to `T&&`: the full check now runs before `value` is touched
at all, and the one `std::move(value)` that actually consumes it runs
only once room is confirmed - preserving "leaves it unconsumed" for both
copyable and move-only `T` alike. Caught by writing the test for exactly
that guarantee (`est::spsc_ring<std::unique_ptr<int>>`, push into a full
ring, assert the source pointer is still non-null) before assuming the
by-value signature was fine; a genuine "test what the doc comment
promises" catch, not a review finding.

**A real second thread, requested separately.** Every other test in this
file simulates the producer by calling `try_push()` directly -
single-threaded, matching `external_event_tests.cpp`'s established
convention. Added one test that doesn't: a real `std::jthread` producer
pushing 2000 values into a 16-slot ring while the loop thread drains it
via `schedule_periodic()`, using the real `platform::interface` (not a
fake clock) so the two threads genuinely interleave over wall-clock time.
Note this codebase's `sanitize` preset is ASan+UBSan only, not
ThreadSanitizer, so this test's own pass/fail says nothing new about the
atomics' correctness beyond what the memory-ordering proof above already
establishes - it exercises the real-OS-thread code path (which CI
otherwise never does), not a substitute for that proof.

**A real toolchain gap, found and fixed rather than worked around.**
`std::jthread` initially failed to *link* (not compile): undefined
references to `std::__1::__atomic_monitor_global`,
`__atomic_wait_global_table`, `__atomic_notify_all_global_table` - hidden
helper functions behind `std::atomic<T>::wait()`/`notify_all()`, which
`std::jthread`'s stop-token machinery uses internally. The first attempt
worked around it (plain `std::thread` instead, `find_package(Threads
REQUIRED)` added to `est/tests/CMakeLists.txt`) rather than root-causing
it - correctly called out as not good enough: nothing about `std::jthread`
or `std::atomic::wait()` should be unusable on a pinned, purpose-built
C++23 toolchain. Root cause, confirmed by diffing `nm -D` (dynamic/
exported symbols) against plain `nm` (every symbol, exported or not) on
the *same* `libc++.so.1.0` install (`/usr/lib/llvm-22/lib/` and
`/usr/lib/x86_64-linux-gnu/` are identical files, byte-for-byte - not a
version-skew issue between two different libc++ builds): the Debian
`libc++-22-dev` package's shared build gives those three helpers hidden
visibility, so they never make it into the `.so`'s dynamic symbol table,
even though the identical-version `libc++.a` *does* contain them (an
archive's member `.o` files carry every global symbol regardless of the
visibility attributes that gate a `.so`'s export table - visibility only
prunes what a shared library exposes, not what a static one contains).
Fixed at the toolchain level, not per-target: `cmake/toolchain-hosted-
linux.cmake`'s `CMAKE_EXE_LINKER_FLAGS_INIT` gained `-static-libstdc++`
(Clang understands this GCC-originated spelling for libc++ too),
statically linking both `libc++` and `libc++abi` into every binary this
project produces instead of depending on the packaged `.so`. Verified
with a standalone repro (`std::jthread` linking against the bare `.so`:
fails with the same three undefined references; against `-static-
libstdc++`: links and runs) before touching the real toolchain file. This
made the `std::thread`/`Threads::Threads` workaround unnecessary -
reverted back to `std::jthread`, and dropped `find_package(Threads
REQUIRED)` from `est/tests/CMakeLists.txt` entirely (this toolchain's
`-pthread` needs, if any, are already satisfied without it - confirmed
by a from-scratch reconfigure with it removed).

**`bugprone-unchecked-optional-access` on the tests, not the source** -
`clang-tidy` doesn't recognize `REQUIRE(popped.has_value())` immediately
before `popped.value()` as a narrowing guard the way a plain `if` would
be. Rather than `NOLINT`-ing every call site, rewritten to compare the
`std::optional<T>` `try_pop()` returns directly against a value or
`std::nullopt` (`ring.try_pop() == 42`, `ring.try_pop() == std::nullopt`)
- `std::optional`'s own `operator==` is well-defined either way it's
engaged, so this sidesteps the question entirely instead of working
around the checker, and reads more directly besides. The one test needing
to actually dereference the popped value (`std::unique_ptr<int>` doesn't
compare against a raw `int`) confirmed the checker doesn't credit
`.value()` either, despite it being bounds-checked - narrowing with
`if (const auto popped = ring.try_pop())`, the same idiom this file's
`while (const auto item = ...)` drain loops already use, satisfies it.

**Docs:** `docs/wiki/Home.md`'s source-location table; a new
`:sync.spsc_ring` node and edge in `docs/wiki/Architecture.md`'s
dependency graph, with a paragraph on why it depends on nothing but
`:check`; a full new "A stream instead of a value" section in
`docs/wiki/Loop-And-Timers.md`, right after `external_event<T>`'s own,
including the composition example, the `.at()`-vs-`noexcept` tension
above, the `T&&`-not-`T` rationale, and a closing note on the one
real-thread test.

**Tests added** (`est/tests/spsc_ring_tests.cpp`): empty ring `try_pop()`
returns `nullopt`; a single push/pop round-trips a value; FIFO order
across several pushes then pops; `try_push()` rejects once full without
overwriting; a full ring accepts again once drained; interleaved push/pop
across many more cycles than `buffer_.size()` stays correct (the classic
place a mod-arithmetic bug in `advance()` would show up); `capacity()`
reflects the constructor argument, not `buffer_.size()`; a trivially
copyable struct works as `T`, not just `int`; moving a `std::unique_ptr<int>`
in and back out round-trips it rather than copying; a value moved into a
full ring is left unconsumed, not moved-from (the `T&&` bug above, caught
by its own test); a full integration test draining a pre-filled ring
through a real `schedule_periodic()` poll loop, matching the composition
example above end to end; and a real `std::jthread` producer racing a
`schedule_periodic()` consumer over wall-clock time (2000 items through a
16-slot ring).

**Verified in the pinned Docker devenv:** 247/247 tests pass (12 new,
including 5 re-runs of the real-thread test to check for flakiness -
none seen); `clang-format`/`clang-tidy` clean over the full tree (every
tension above resolved, not suppressed); 214/214 tests pass under the
`sanitize` preset (ASan+UBSan) too; `diff-cover` coverage gate against
`main` at 97% (`spsc_ring.cppm` itself still at 100%).

---

### Issue #76: `examples/multicolor_larson_scanner` - three independent scanners, a real controller thread, composing the last several issues' own primitives

Requested: a new example - three independent Larson ("KITT"/Cylon)
scanners, one per RGB channel with its own speed/decay, animating into
a shared LED buffer; a controller reading live string commands; an
output module rendering the buffer as a colored terminal strip. Planned
out (`/plan`) before any code, with two steers during that planning
that shaped the final design more than the issue text alone did:

**The controller is a real `std::jthread`, not another
non-blocking-poll coroutine.** `examples/spreadsheet/src/
spreadsheet_io.cppm`'s `getline_async()` (issue history, well before
this one) polls a non-blocking fd from a coroutine specifically to
avoid blocking the one thread `est::loop` runs on. Asked to use a
genuine second thread instead: `input_thread` blocks on plain
`std::getline(std::cin, line)` (fine - it isn't the loop thread),
pushes each parsed command into an `est::spsc_ring<command>` (issue
#96), and bumps an `std::atomic<std::size_t>` counter once per push.

**Draining reacts to an event, not a fixed poll.** Second steer: don't
drain the ring inside a periodic callback's own body. `docs/wiki/
Loop-And-Timers.md`'s "A stream instead of a value" section (written
for issue #96) had already named this exact optimization without
implementing it - *"layer `est::external_event<std::size_t>` over a
monotonic write-counter bumped alongside every successful
`try_push()`, only draining when that counter actually moved."* This
example is that optimization, for real: a 20ms periodic timer calls
only `external_event<std::size_t>::poll()` (an O(1) load+compare) on
the write-counter; a small coroutine, `drain_commands()`, `co_await`s
the bridge's own `wait()` and only touches the ring once actually
woken. Three primitives from three different issues - `schedule_periodic()`
(#67-era), `spsc_ring<T>` (#96), `external_event<T>` (#74) - compose at
one call site exactly as each was designed to, without any of them
knowing about the others.

**Module split** mirrors `examples/spreadsheet/`'s own
`spreadsheet.cppm`/`spreadsheet_io.cppm` divide: `src/larson_scanner.cppm`
is pure logic with no `est::` dependency at all (`scanner_channel::tick()`,
`led_buffer`, `render()`, `parse_command()`, `apply()`) and is
independently unit-tested; `main.cpp` is the thin wiring layer (the
three periodic timers, the ring, the bridge, the input thread,
`drain_commands()`). `scanner_channel::tick()` decays every pixel by
`decay`, pins the *current* position to full brightness, then advances
and bounces off either end - order matters, since a stationary
scanner (`speed == 0`) needs to stay pinned rather than decay away.
`render()` renders each pixel as a 24-bit ("truecolor") ANSI-colored
Unicode block glyph (9 height levels, 0 = a literal space so a fully
unlit pixel renders as blank, not a sliver), colored from that pixel's
own R/G/B intensities, height picked by the brightest of the three.
The command protocol is `"<channel> <param> <value>"` (`channel` ∈
`{r, g, b, all}`, `param` ∈ `{speed, decay}`, e.g. `"r speed 0.5"`),
plus a literal `"quit"` line; EOF on stdin synthesizes the same quit
command, so both shutdown paths converge on one code path
(`drain_commands()` popping a quit command calls `loop.stop()`).
Verified manually (piped commands, a real run): both shutdown paths
exit promptly with no hang waiting on `input_thread`'s join, and a
live `"r speed 2.0"` visibly changes red's own trail independently of
green/blue's.

**Real `clang-tidy` findings, each fixed rather than suppressed without
reason:**
- `readability-identifier-naming` on the three new enum types
  (`channel_selector`/`param_kind`/`command_kind`) - this codebase's
  own established convention for enum *type* names is CamelCase
  (`spreadsheet::Verb`'s own doc comment: *"this project's first enum,
  everything else here otherwise matches the codebase's own
  lower_snake_case"*); renamed to `ChannelSelector`/`ParamKind`/
  `CommandKind`, enumerators stayed lowercase, matching `Verb`'s own
  split.
- `readability-math-missing-parentheses`/`bugprone-incorrect-roundings`
  on a hand-rolled `* 8.0F + 0.5F` rounding trick in the glyph-selection
  helper - replaced with `std::lround()`, the fix both diagnostics
  themselves suggested.
- `cppcoreguidelines-non-private-member-variables-in-classes` on
  `scanner_channel::speed`/`decay` (public, mutable, sitting alongside
  private `intensity_`/`position_`/`direction_`) - a real, deliberate
  mix (the controller needs cheap direct writes; a getter/setter pair
  buys no actual encapsulation), `NOLINTNEXTLINE`-suppressed with a
  comment, matching `est::future_state<T>`'s own `state_`/`owner_`
  members (`est/src/future.cppm`) for the identical reason - not a new
  precedent, the second use of an already-established one.
- `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` on
  `intensity_[pinned]` inside `tick()` - `pinned` is provably always
  `< intensity_.size()` by construction (`position_` starts at 0 and
  every `tick()` call clamps it back into range before returning), the
  same shape `est::spsc_ring<T>`'s own unchecked accesses already
  document (issue #96) - `NOLINTNEXTLINE` plus the invariant, not a
  silent suppression.
- `bugprone-easily-swappable-parameters` on `scanner_channel`'s
  3-argument constructor (`width`, two adjacent `float`s) -
  `NOLINTNEXTLINE`, since it's always called positionally from
  `led_buffer`'s own constructor with the same argument order every
  time; a named-parameter redesign would be ceremony a small, internal
  type doesn't need.
- `bugprone-exception-escape` on `main()` - a real gap, not a false
  positive: `est::spsc_ring<T>`'s/`led_buffer`'s own allocations,
  `schedule_periodic()`'s precondition checks, and `std::jthread`
  construction all originally sat *outside* the `try`/`catch(...)`
  block, reachable without being caught. Fixed by moving the entire
  body inside the `try` - nothing throwing is reachable from `main()`
  outside it any more.

**Follow-up: `tick()` takes an explicit `dt`, not an implicit "one step
per call."** Review feedback after the initial PR: each scanner's own
physics should be genuinely time-independent - `tick()` shouldn't
assume it's always called at exactly the periodic timer's own
interval, only that the *scheduling* of when it's called stays a
regular `schedule_periodic()` timer. Both `scanner_channel::tick()`
and `led_buffer::tick()` now take a `std::chrono::duration<float> dt`
parameter, and `speed`/`decay` were reinterpreted from "per call" to
genuinely physical per-second quantities: `speed` stays pixels/second
(so position advances by `speed * dt.count()`, a plain linear scale),
but `decay` - a multiplicative per-pixel falloff - can't scale
linearly with `dt` and stay correct, since exponential decay compounds
*multiplicatively* over time (`v(t) = v(0) * decay^t`). `tick()`
applies it as `std::pow(decay, dt.count())` instead, so a pixel's trail
length in real time stays constant regardless of how often `tick()`
happens to be called - two `0.5s` ticks fade a pixel by the same total
factor as one `1.0s` tick (`pow(d, 0.5) * pow(d, 0.5) == pow(d, 1.0)`).
`main.cpp`'s own physics timer is unchanged in spirit - still a plain
`schedule_periodic()` at a fixed interval - except that interval is now
a named `tick_interval` constant reused as both the timer's own period
and the `dt` argument passed to `buffer.tick()` each time, rather than
`tick()` assuming any particular cadence on its own. Two real build
errors surfaced fixing this: a `constexpr` interval still needs
explicit capture (`[&buffer, tick_interval]`, not `[&buffer]`) once
it's ODR-used inside the lambda body at runtime; and
`Catch::Matchers::WithinAbs` is `double`-only, so passing it `float`
literals tripped this codebase's `-Wdouble-promotion -Werror` - the new
tests use a plain `float` epsilon comparison instead, matching every
other assertion in this test file. Two tests added to prove the
compounding math directly rather than just trusting the derivation:
one ticking `1s` then two `0.5s` steps and checking the decayed
intensity matches ticking `1s` once; one comparing two channels
covering identical ground at different `dt` granularities (`2px/s` at
`1s` steps vs. `4px/s` at `0.5s` steps) and checking their bounce
sequences stay identical at every step. Every pre-existing test call
site was updated to pass `1s` as `dt` (`pow(decay, 1) == decay`, so the
original numeric expectations needed no changes, only the new
argument).

**Second follow-up: the est-dependent wiring moved out of `main.cpp`,
into its own module, behind an interface with no coroutines in it.**
Review feedback again: `main.cpp` had grown into the one place that
both drove `est::loop`/`est::schedule_periodic()`/`est::spsc_ring<T>`/
`est::external_event<T>` *and* decided how to render/print - asked to
separate those concerns further, with the drain/render wiring moving
into a module of its own (mirroring `examples/spreadsheet/`'s own
`spreadsheet`/`spreadsheet_io` split a second time, this time inside
the same example rather than across two of them) and, specifically,
with that module's own exported surface never mentioning
`est::future<T>` - only a plain `push_command()`, a blocking `loop()`,
and a render callback. New module `larson_scanner_app.cppm` (top-level,
alongside `larson_scanner.cppm`, not a partition of it - same reasoning
`spreadsheet_io` is a separate module from `spreadsheet` rather than a
partition) exports one class, `app`: it owns the `est::loop` and every
timer/queue/event/coroutine around it internally (the physics-tick,
render-trigger, and command-poll periodic timers; the
`spsc_ring<command>`/`external_event<std::size_t>` handoff;
`drain_commands()`, now a *private* member coroutine, never named
outside the class), and exposes exactly three things: `push_command()`
(thread-safe/external-context, spins via `try_push()` the same way the
old input-thread lambda did), `loop()` (blocks the calling thread,
registers `current_loop()`, starts the three timers and the drain
coroutine, then calls `est::loop::run()` - no `future<T>` anywhere in
its own signature, `drain_commands()`'s coroutine frame is just its own
local variable), and a `render_callback` (`std::function<void(const
led_buffer&)>`, set once at construction) invoked once per render tick
with the buffer itself, not a string - deliberately *not* the
UTF8/ANSI-rendering half, which stays exactly where it already lived,
`larson_scanner::render()` in the pure module, called from inside
whatever callback the caller hands `app`'s constructor. `main.cpp`
shrank to matching size: install the platform, build an `app` with a
render callback that calls `render()` and `std::println()`, spawn the
input `std::jthread` calling `push_command()`, call `loop()` - no
`est::loop`/`schedule_periodic()`/`spsc_ring`/`external_event` visible
in this file at all any more. `larson_scanner_app` is a separate
CMake library target from `larson_scanner` (linking `est::est` and
`larson_scanner::larson_scanner`), the same "one half is pure and
est-free, the other isn't, and nothing should have to link the second
just to use the first" reasoning `examples/spreadsheet/CMakeLists.txt`
already established for its own two targets - `larson_scanner_tests`'s
pure-logic cases still only link `larson_scanner::larson_scanner`.
Testing this new class needed a real installed `platform::interface`
(a real or fake `est::loop` is otherwise inert), so the test binary
gained its own `test_main.cpp` (mirroring
`examples/spreadsheet/tests/test_main.cpp`) and switched from
`Catch2::Catch2WithMain` to `Catch2::Catch2`; one clang-tidy finding
fell out along the way -
`performance-move-const-arg`/`bugprone-use-after-move` on
`push_command()`'s original `std::move(cmd)` retry loop, flagging (not
incorrectly, just unable to see across loop iterations) that a rejected
`try_push()` never actually moves from its argument - fixed by going
back to a fresh `command{cmd}` copy per attempt, costing nothing extra
since `command` is trivially copyable anyway.

**Tests added for the `app` split**
(`examples/multicolor_larson_scanner/tests/
larson_scanner_app_tests.cpp`): one fully deterministic case using a
local fake `platform::interface` (mirroring
`est/tests/spsc_ring_tests.cpp`'s own fake-clock pattern) whose
`sleep_until()` fast-forwards instead of blocking - a command is queued
via `push_command()` before `loop()` even starts, and the render
callback itself both observes the applied effect and pushes the `quit`
command that stops the loop, sidestepping any dependency on tie-broken
timer-firing order between the three periodic chains; one case with a
genuine `std::jthread` producer calling `push_command()` from a real
second OS thread against the real platform (needed here for the
identical reason `spsc_ring_tests.cpp`'s own real-thread test needs the
real platform: a fake clock's `sleep_until()` never actually blocks,
starving the producer thread of any real wall-clock window to run in).

**Docs:** this entry (examples aren't documented in `docs/wiki/`, which
is scoped to `est`/`estext` themselves, not `examples/`).

**Tests added** (`examples/multicolor_larson_scanner/tests/
larson_scanner_tests.cpp`, all against the pure `larson_scanner`
module, no I/O): `scanner_channel::tick()`'s bounce sequence made fully
deterministic via `decay = 0` (the lit pixel traces `0, 1, 2, 1, 0` for
a width-3 strip); decay actually fading a pixel the scanner has moved
away from; a stationary scanner (`speed == 0`) staying pinned rather
than decaying; `led_buffer::tick()` ticking three channels
independently once their speeds diverge; `render()` against three
known intensity patterns (fully lit, fully unlit, half-lit) checking
the exact escape sequence and glyph; `parse_command()` round-tripping
every channel/param spelling plus `"quit"`, and rejecting every
malformed shape (wrong token count, non-numeric value, unknown
channel/param, trailing garbage); `apply()` exercised against each of
the four `ChannelSelector` values individually, not just a
representative pair, once diff-coverage caught the `green`/`blue`
switch cases going otherwise unexercised; plus, from the `dt` follow-up
above, decay compounding correctly across a split `dt`
(`pow(decay, dt)`) and `speed * dt` determining distance moved rather
than the call count; plus, from the `app`-split follow-up above, its
own two `larson_scanner_app_tests.cpp` cases.

**Verified in the pinned Docker devenv:** 264/264 tests pass (17 new
overall, 2 from the `dt` follow-up, 2 from the `app`-split follow-up);
`clang-format`/`clang-tidy` clean over the full tree (every finding
above fixed, not suppressed without reason); `diff-cover` coverage gate
against `main` at 97% over the whole PR diff (the few misses are
`push_command()`'s never-actually-full-ring retry branch and a couple
of never-triggered `platform::interface` stub overrides in the test
fakes - both already comfortably clear of the 80% gate, and neither
CI's own coverage step, which only measures `est_tests`, actually
requires this at all - this remained a self-imposed check, same as
every prior entry in this PR); a manual run (piped commands, spaced
out to actually observe the animation update between them this time)
confirmed the animation, live command handling, and both shutdown
paths (`quit` and EOF) all work with no hang. The `sanitize` preset's
own test count (214/214, unchanged) correctly excludes `examples/` by
design (see this file's own Dockerfile/CI notes) - this example's
tests aren't part of that run, matching every other `examples/*/tests`
binary in this codebase.

**Deferred, not this PR:** the WebAssembly+single-HTML-page stretch
goal from the issue's own follow-up comment (sliders drive scanner
inputs, a Wasm build runs the animation, CSS "LEDs" show the output -
no React, viewable straight from a GitHub Pages-hosted single HTML
file). Corrected during planning: not necessarily an Emscripten
dependency - this toolchain's own Clang has a native `wasm32` backend
(`--target=wasm32-unknown-unknown`/`wasm32-wasi`) that emits `.wasm`
directly, no POSIX emulation layer, no auto-generated JS glue. That's a
closer fit to this codebase's own architecture than Emscripten would
be: `est::platform::interface` is already the seam `docs/wiki/
Architecture.md` describes existing so "a future bare-metal backend
would be its own similarly separate module," the same as
`estext::hosted_stdcpp` is for hosted Linux today - a minimal wasm
backend implementing that interface would plausibly need no POSIX
layer at all, genuinely closer to "bare metal" than "hosted." Left to
scope as its own issue when picked up.

### Issue #99: WebAssembly build, M0 spike - a real CMake gap, then a real fix

Issue #99 (github.com/andijcr/async_experiments/issues/99) reopened the
WebAssembly stretch goal deferred out of issue #76/PR #98, adding a CSS
box-blur glow effect for the LED output. Planned out (`/plan`), then
steered mid-planning toward a more ambitious design than the plan's own
first draft: rather than bypassing `est::loop` entirely and letting JS
drive ticks directly (this plan's own original, lower-risk direction),
see whether `est::loop` can genuinely *block*, the way it's designed to,
inside a Web Worker - `Atomics.wait()` (the one real OS-level blocking
primitive JS exposes) is only callable on a Worker thread, so a
`est::platform::interface` wasm backend whose `sleep_until()` blocks via
`Atomics.wait()` is only achievable there, which in turn - since a
blocked Worker can't process `postMessage` at all - means genuine
WebAssembly threads (one shared `WebAssembly.Memory`, `est::spsc_ring<T>`/
`est::external_event<T>` reused for real, exactly as designed) rather
than a plain JS-driven bypass. Framed as two upfront spikes (M0: does
`import std;` work for a `wasm32` cross-compile at all; M0.5: shared
memory across two instances of one module) deciding feasibility, with an
explicit stop condition per steer: if a spike fails, stop and document
findings on the issue - no silent fallback to the simpler bypass design.

**M0, first attempt: a real, well-diagnosed blocker.** Fetched a pinned
wasi-sdk-34 release purely for its `share/wasi-sysroot` (renamed
`wasm32-wasi` → `wasm32-wasip1`/`wasm32-wasip1-threads` since this
project's toolchain was last touched) and its `lib/clang/23` resource
tree (compiler-rt builtins - this project's own Clang 22 package ships
none for wasm32). Root-caused, methodically, several real (non-workaround)
fixes along the way - `CMAKE_CXX_FLAGS_INIT` needing `--target=`/
`--sysroot=` explicitly (CMake's own libc++-detection probe doesn't add
them), wasi-sdk-34's classic headers living only under per-target/
per-eh-variant subdirectories, `-mexception-handling`/
`-D_WASI_EMULATED_SIGNAL` for wasi-libc's own `<csetjmp>`/`<csignal>`
guards - before hitting the actual blocker: CMake 3.31 (this project's
pin) resolves `import std;`'s module sources via `clang++
-print-resource-dir`, which reports the compiler's own *built-in*
default unconditionally, ignoring any `-resource-dir=`/`--sysroot=`/
`--target=` passed alongside it - so it always compiled the *host's*
own libc++-22 `std.cppm` against wasm32 flags instead of the
wasi-sysroot's own, with no toolchain-file-level fix. Posted the full
diagnostic chain to issue #99 and stopped there, per the stop condition
- no Plan B built.

**Then a real fix: CMake itself, not a wrapper.** Asked directly whether
a newer CMake could help, or whether a filesystem-trick wrapper
(relocating/hardlinking the compiler binary so its own default
resource-dir traversal lands on the right files) was needed instead.
Researched CMake's own gitlab issue tracker and found `import std;`'s
module-source resolution was substantially reworked upstream: **CMake
4.2** (commit landed 2025-09-11) added `CMAKE_CXX_STDLIB_MODULES_JSON`
(an explicit override for the exact `-print-resource-dir`-based
detection that broke M0) and `CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES`
(extra `-isystem` paths fed into the stdlib-detection probe itself,
fixing the earlier "Only `libc++` is supported" failure mode too) - both
sanctioned, toolchain-file-settable variables, no CMake source patching,
no relocated/hardlinked compiler, no symlink farm. Verified for real
before committing to it: fetched CMake 4.4.0, retried the exact M0
translation unit (`import std;` for `wasm32-wasip1-threads`) with the
two new variables set, and it linked - a genuine `.wasm` binary (`\0asm`
magic bytes confirmed), using the wasi-sysroot's own module sources this
time, not the host's.

**Bumped the project's own pinned CMake** (`docker/Dockerfile`'s
`ARG CMAKE_VERSION`, 3.31.0 → 4.4.0) on the strength of that spike -
this fixes a real gap for the *existing* hosted build too, not just the
wasm effort, so it's landing as its own change rather than staying
wasm-scoped. `cmake/toolchain-hosted-linux.cmake`'s own
`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` UUID moved to CMake 4.4.0's current
activation value (`f35a9ac6-...` - CMake bumps this UUID whenever the
experimental feature's own shape changes, so it's tied to the pinned
CMake version, not portable across versions on its own).

**One real regression surfaced by the bump, fixed, not suppressed
without reason:** CMake ≥4.2's revised `import std;` machinery compiles
libc++'s own `std.cppm` as a synthetic *per-consuming-target* build
product - and that synthetic target turned out to inherit a consuming
target's own `PRIVATE` compile options (confirmed via the actual failing
command line), meaning `est_set_warnings()`'s own `-Werror` started
reaching `std.cppm`'s unconditional (not gated behind `-Wall`/`-Wextra`)
`-Wreserved-module-identifier` diagnostic on `export module std;` -
libc++'s own code, not anything this project can fix at the source.
Added `-Wno-reserved-module-identifier` to
`cmake/toolchain-hosted-linux.cmake`'s own `CMAKE_CXX_FLAGS_INIT`
(toolchain-wide, matching `-stdlib=libc++`'s own "every target needs
this" placement, and ordered so it isn't re-escalated by a later
target's own `-Werror`, unlike a hypothetical `-Wno-error=` spelling
would have risked).

**Verified in the pinned Docker devenv**, image rebuilt with CMake
4.4.0 (network constraints in this environment blocked a from-scratch
`docker build` at the pre-existing, unrelated `apt.llvm.org` step - the
same LLVM/Clang 22 install this project already pins, unchanged by this
work - so the already-built image was patched in place with the new
CMake and re-tagged; a real CI run, with normal network access, builds
this Dockerfile from scratch as usual): `cmake --preset default` +
`cmake --build --preset default` clean; `ctest --preset default` 264/264;
`clang-format --dry-run --Werror` clean; `cmake --preset ci` +
`cmake --build --preset ci` clean (the `-Wreserved-module-identifier`
fix confirmed load-bearing here specifically); `clang-tidy -p build/ci`
clean over the whole tree; `ctest --preset ci` 264/264; the coverage gate
(no source lines in this infra-only diff, so nothing for it to check);
`cmake --preset sanitize` + `cmake --build --preset sanitize` +
`ctest --preset sanitize` 214/214.

**M0 retry and M0.5, against the real bumped devenv image: both hold.**
`import std;` for `wasm32-wasip1-threads` now succeeds (valid `.wasm`,
`\0asm` magic verified). Stretch-checked further by compiling all 19 of
`est`'s own `.cppm` partitions (not just a synthetic TU) - also
succeeds end-to-end, once switched the wasm32 toolchain file to the
wasi-sysroot's `eh` (exceptions-enabled) variant and dropped
`-fno-exceptions -fno-rtti`: `future.cppm`'s `concrete_continuation::
run()` genuinely uses `try`/`catch` to propagate exceptions from
continuations, which the original `import std;`-only spike's
`-fno-exceptions` simplification can't coexist with. M0.5 (shared
memory across two independently-instantiated copies of one module,
static init running exactly once): a minimal `est`-free `.wasm`,
instantiated once as a simulated main thread and once in a real
`node:worker_threads` Worker sharing one `WebAssembly.Memory({shared:
true})` - the worker's own first read of a non-zero-init global came
back correctly initialized (not zero, not garbage), and writes made
from the worker were immediately visible to the main-thread instance's
reads. Both results posted to issue #99. Per the stop condition, both
spikes holding means the next step is the real Plan A build - the
`estwasm` platform backend, the Worker/shared-memory wiring, the HTML
page - as its own branch/PR closing issue #99, not yet started as of
this entry.

**Plan A build, started for real: blocked on genuine Clang 22.1.8
wasm32 backend instability, not a flags problem.** Wrote the real
pieces - `estwasm` (a third `est::platform::interface` backend,
`estext`-sibling, routing `now()`/`sleep_until()`/randomness/
diagnostics through `import_module("env")` JS imports, `sleep_until()`
blocking via `Atomics.wait`), a standalone `examples/
multicolor_larson_scanner/web/` CMake project reusing `est`/
`larson_scanner`/`larson_scanner_app` unmodified, `wasm_exports.cpp`
(`boot()`/`push_command()`), and `docker/Dockerfile`'s wasi-sdk sysroot
fetch. Building it for real (not just `import std;`/`import est;` in
isolation, M0's own scope) surfaced two independent, reproducible Clang
22.1.8 wasm32-backend crashes on genuine, unmodified application code:

1. Any real C++20 coroutine body (confirmed with a minimal, `est`-free
   `co_await std::suspend_never{}` repro, and with the real
   `larson_scanner_app::drain_commands()` - `future<T>` itself is a
   coroutine return type, so this isn't avoidable) segfaults Clang's
   `coro-split` pass when `-mexception-handling` is active *and*
   optimizations are off (`-O0`, this toolchain's implicit default).
   `-mexception-handling` itself isn't optional either - the `eh`
   sysroot variant's own precompiled `libc++abi.a` requires it
   (`__cpp_exception`/`_Unwind_RaiseException` aren't provided any
   other way on this target), and `future_state<T>`'s own exception
   propagation needs the `eh` variant.
2. Raising the optimization level to `-O1`/`-O2` (which does avoid
   crash 1) instead segfaults a *different* pass (`EarlyCSE`,
   `llvm::simplifyInstruction`) compiling `larson_scanner.cppm`'s
   `parse_command()` - plain `std::istringstream`/`operator>>` usage,
   nothing exotic, no coroutines involved at all.

Two unrelated crashes, in two different LLVM passes, on two different
and unremarkable pieces of real code, surfacing only once actual
optimization-level/flag combinations a real build needs were tried
together (M0's own spikes never exercised a real coroutine body or
`std::istringstream` - both passed because they were narrower than the
actual application). That pattern - each fix uncovering a new, distinct
crash elsewhere - is read as genuine wasm32-backend instability in this
exact pinned Clang snapshot for real C++23-modules code, not a
toolchain-flag gap this project can tune its way around the way M0's
own `-print-resource-dir` gap was.

Per the stop condition (docs/PLAN.md's own steer, and the comment
already posted to issue #99): stopping here rather than stacking
further per-file/per-optimization-level workarounds (which the pattern
above suggests would just keep surfacing new crashes one file at a
time) or falling back to Plan B. The `estwasm`/`web/`/Dockerfile
changes written during this attempt were reverted rather than
committed - they don't produce a working artifact, and this project's
own standards (CLAUDE.md) are against landing known-fragile workarounds.
Findings posted as a follow-up comment on issue #99; the issue stays
open for a future, separate decision (revisit once a newer Clang
snapshot is pinned and these crashes are checked against it, or
deliberately pursue Plan B instead). No branch, no PR for this attempt.

**Confirmed both crashes are open, unfixed upstream LLVM bugs, not a
Clang-22-specific gap.** Checked whether a newer Clang would fix either:
both match known, currently open issues (llvm/llvm-project#208409 for
the coroutine crash - a regression from LLVM 18, reproduced on LLVM
main/23.0.0git itself; llvm/llvm-project#148550 for the `EarlyCSE`
crash, same trigger shape, opened July 2025, still open). Neither is
something a Clang version bump routes around - both live in shared
WebAssembly-backend/optimizer passes any similarly recent LLVM build
carries. Posted as a follow-up comment on issue #99.

**The actual sidestep: `-fno-exceptions`, and a small, principled
change to `est` itself.** Neither crash's trigger condition
(`-mexception-handling`'s real wasm exception-handling target-feature)
is reachable at all under `-fno-exceptions` - verified by rebuilding
`est`'s own module tree that way: 44 of 48 partitions succeeded with
zero crashes, the only failure being a genuine, expected compile error
("cannot use 'try' with exceptions disabled") at `future.cppm`'s own
`concrete_continuation::run()`. That's not a real loss of
functionality, though: `-fno-exceptions` makes `throw` illegal
*everywhere* in the TU, so nothing - including a `.then()` callback's
own body - can ever throw in the first place, making that `catch`
block genuinely dead code on this target already. Gated all four real
`try`/`catch` sites in `est`'s own partitions (`future.cppm` ×2,
`timer_periodic.cppm`, `platform.cppm`) behind the standard
`__cpp_exceptions` feature-test macro instead of removing them - the
exceptions-enabled hosted build keeps its existing behavior unchanged
(full pipeline re-verified: 264/264 default tests, clean format/tidy,
214/214 sanitize), and the wasm32 build now compiles the real
`try`/`catch` code path out entirely rather than needing it deleted.

**Plan A, built and verified for real, end to end.** With that fix,
rebuilt `estwasm`/`examples/multicolor_larson_scanner/web`/the
Dockerfile's wasi-sdk sysroot fetch against the `noeh` sysroot variant
+ `-fno-exceptions` (`cmake/toolchain-wasm32.cmake`) - all 64 build
steps succeed, producing a valid `.wasm`. Two more real, integration-
level bugs surfaced only once actually *running* the result (not just
compiling it), caught by inspecting the built binary and a real
two-instantiation test rather than assumed away:

- The module defined (and exported) its own memory by default -
  `-Wl,--shared-memory` alone doesn't force an *imported* memory, so
  two separate `WebAssembly.instantiate()` calls would each have
  gotten their own independent copy, silently defeating the entire
  shared-memory design. Fixed with `-Wl,--import-memory` plus explicit
  `-Wl,--initial-memory=`/`-Wl,--max-memory=` (shared memory requires a
  fixed max), confirmed via `WebAssembly.Module.imports()`/`exports()`
  against the actual built binary.
- `_initialize()`'s disassembly shows the shared-memory data/ctor init
  is guarded by an atomic compare-and-swap whose "not first" branch is
  a literal `unreachable` trap, not a wait - meaning exactly one
  instantiation (the primary, before any Worker starts) may call
  `_initialize()`; every other instantiation sharing that memory must
  skip it entirely, or it deterministically traps. Not something a
  spec reading alone would have surfaced - found by running a real
  two-instantiation Node test (`node:worker_threads`, mirroring the
  browser's own Worker/main-thread split) and reading the crash.

With both fixed, the real end-to-end test passes: the Worker's
`est::loop` genuinely blocks via `Atomics.wait()`, the main thread's
`push_command()` (a real, synchronous C++ call, not `postMessage`)
visibly changes the animation through shared memory, and
`js_worker_ready()`'s one-time handshake hands the main thread stable
pointers into `led_buffer`'s own intensity arrays. Also verified in a
real, unflagged headless Chromium (Playwright) end to end: the
`coi-serviceworker.js` polyfill (vendored, MIT-licensed, unmodified)
genuinely establishes `crossOriginIsolated` on its own - including
exercising its COEP-degrade fallback path, the same one a real GitHub
Pages deployment needs since Pages can't set custom COOP/COEP response
headers - with the LED strip visibly animating and the glow effect
(CSS custom properties + `box-shadow`, no per-frame string building)
rendering as a soft, brightness-scaled halo rather than a flat block.

**What shipped**, all reusing `larson_scanner`/`larson_scanner_app`
*unmodified* per issue #99's own point: `estwasm` (a third
`est::platform::interface` backend, `estext`-sibling module, routing
`now()`/`sleep_until()`/randomness/diagnostics through
`import_module("env")` JS imports); `examples/
multicolor_larson_scanner/web/` (a standalone CMake project - its own
`project()`, its own `wasm32` preset - plus `index.html`/`main.js`/
`worker.js`/`env_shim.js`/`wasi_shim.js`/the vendored
`coi-serviceworker.js`); `docker/Dockerfile`'s pinned wasi-sdk sysroot
fetch (two small release assets, not the full SDK - never wasi-sdk's
own bundled Clang); a Node-based CI smoke test
(`examples/multicolor_larson_scanner/web/tests/`) exercising the real
Worker/main-thread split headlessly, plus an import-section check
against a fixed allowlist; a new `.github/workflows/ci.yml` job and a
new `.github/workflows/pages.yml` deploying the page to GitHub Pages
on push to main (needs a one-time manual "enable Pages for this repo"
step outside CI, per that workflow's own top comment).

---

### Issue #56: `future<T>` cancellation (stop_token-style) (done)

Issue #56 (github.com/andijcr/async_experiments/issues/56) asked for a way
to ask a still-pending `future<T>`/coroutine to stop early - the only
thing resembling this before was abandonment (`future_state<T>`'s
destructor-time waiter drain), which only helps when *nothing* will ever
resume the coroutine again (the owning object dying), not when the thing
being waited on is still perfectly alive and a caller just wants to give
up on it. The issue predates `when_any()`/`when_all()`/
`when_any_succeeds()` (all three landed after it was filed - both
`when_any()`'s and `when_all()`'s own entries above explicitly deferred
cancellation to this issue: "this codebase has no cancellation mechanism
at all") - the design here was updated to build on top of those rather
than the empty landscape #56 was originally written against, and posted
as an issue comment before any code, mirroring issue #99's own
convention.

**`est::make_failed_future<T>(exception)`** (`est/src/promise.cppm`) is
the missing failure-case sibling to the existing `make_ready_future<T>()`
- both the `with_stop()` fast path and the token-aware sleep overloads'
fast path (below) need a fresh, already-*failed* future built from
scratch, and there was previously no one-line way to do that.

**`est::stop_source`/`est::stop_token`/`est::operation_cancelled`** (new
partition `est:sync.stop_token`, `est/src/sync/stop_token.cppm`) are
built directly on `make_promise_future<void>()` + `future<void>::clone()`
rather than `:sync.event`'s `one_shot_event` - `:sync.event` already
depends on `:promise`, and the token-aware sleep overloads (below) need
`:promise` itself to sit *above* `:sync.stop_token`, so routing through
`:sync.event` would have closed a cycle
(`:promise → :sync.stop_token → :sync.event → :promise`). `stop_token::
stopped()` returns a `future<void>` any number of independent consumers
can `co_await`/`then_fast()`, via the same `clone()` a multi-consumer
signal already needed with zero new machinery; `request_stop()` is
idempotent, matching `one_shot_event::set()`'s own "independent
cancellation sources safely racing to fire the same signal" contract,
reimplemented directly here since `stop_state` doesn't build on
`one_shot_event`. Cancellation itself is a second, distinct, exported
exception type (`operation_cancelled`) flowing through the exact same
`set_exception()` channel `abandoned_exception` already uses - no change
to `future_state<T>`'s pending/value/exception representation or any
`.then()` dispatch branch.

**`est::with_stop<T>(future<T> operation, const stop_token&)`** (new
partition `est:with_stop`, `est/src/with_stop.cppm`) races `operation`
against `token.stopped()` using the identical `shared_ptr<state>` + two
`then_fast()` racers shape `when_any()` already established, generalized
to carry a real `T` through. Its documented, honestly-scoped limitation
matches `when_any()`/`when_all()`/`when_any_succeeds()`'s own already-
accepted one: it stops the *caller* from waiting further, but doesn't
eagerly free `operation` itself, which keeps running in the background
until it completes on its own (a harmless no-op via a `done` guard by
then). Eagerly freeing an arbitrary suspended coroutine or queued
`mutex::lock()`/`counting_event::wait()` waiter would need
`intrusive_list<T>::remove()` from the middle, which doesn't exist
(`enqueue`/`dequeue`/`drain` only) - a broad, invasive change touching
every list in the codebase, explicitly scoped out as a follow-up rather
than folded into this already-large change, the same way `when_any()`/
`when_all()` themselves deferred cancellation to this issue.

**Timed cancellation is the one case that's genuinely eager**, not just
"stop watching": `est::loop::cancel_timer(timer_id)` (new, alongside a
`schedule_timer()` that now returns the `timer_id` it used to discard)
pulls a still-pending `sleep_resume_node` out of `loop`'s timer queue
early and completes it via the exact same `abandon()`-then-destroy path
`drain_pending()` already used at teardown - `timer_queue::cancel(id)`
itself already existed and worked, it just wasn't exposed for one entry
on demand. `est::sleep_for(delay, const stop_token&)`/`est::sleep_until
(deadline, const stop_token&)` (`est:with_stop`, not `:promise` - putting
them in `:promise` would itself have needed `:promise → :sync.stop_token`,
closing the same cycle described above) build on this: a fast path
matching `with_stop()`'s own when already `stop_requested()` (no timer
ever scheduled), otherwise a racer that, on the token winning, calls
`loop.cancel_timer()` and completes the caller-visible future with
`operation_cancelled` - real, immediate reclamation of the timer, not a
flag checked later.

**Tests added**: `make_failed_future<T>()`/`make_failed_future<void>()`
(`est/tests/loop_tests.cpp`, alongside the existing `make_ready_future()`
tests); `est/tests/stop_token_tests.cpp` (idempotent `request_stop()`,
`stop_requested()` before/after, independent `get_token()` copies all
observing one signal, `stopped()` immediate vs. genuine coroutine
suspend/resume); `est/tests/with_stop_tests.cpp` (normal completion
forwarding value/failure, `request_stop()` before completion resolving
`operation_cancelled` with the operation's later completion proven a
no-op via a manually-held `promise<T>`, `request_stop()` *after*
completion also proven a no-op the other way around, an already-
`stop_requested()` token short-circuiting without ever touching the
operation); timed-cancellation tests in `loop_tests.cpp` proving
`cancel_timer()` genuinely removes a pending timer rather than merely
losing the race (a fake platform's clock never advancing anywhere near a
deliberately huge deadline once cancelled, plus `counting_resource`
allocation-count balance) and that `cancel_timer()` on a stale/unknown id
returns `false` rather than asserting. Full devenv pipeline
(`cmake --preset default/ci/sanitize` + `ctest` + `clang-format` +
`clang-tidy` + the new-code coverage gate, all real, tested logic -
98% diff coverage) - unlike the immediately-preceding wasm work's
infra-only diffs, this has real logic and real coverage to show for it.

**Explicitly out of scope**: eager cancellation of a queued
`mutex::lock()`/`counting_event::wait()` waiter (the `intrusive_list<T>`
gap above) - worth its own follow-up issue, not built here; and wiring
`est::stop_token` into `examples/spreadsheet/src/spreadsheet.cppm`'s
`resolve_blocking()`/`GET BLOCKING` protocol (the issue's own concrete
motivating case, named only as motivation) - actually changing that wire
protocol to accept a timeout is a separate protocol/UX decision (what
syntax, what error response) deserving its own issue rather than being
folded in silently here.

`docs/wiki/Architecture.md`'s partition DAG, `docs/wiki/Coroutines.md`
(a new section distinguishing abandonment from `stop_token`-based
cancellation), `docs/wiki/Loop-And-Timers.md` (`cancel_timer()`), and
`docs/wiki/Home.md`'s "where to look" table were all updated to match.

**`examples/digit_recall`**, added afterward on the same PR: a small
terminal reflex game (`sleep_sort`/`spreadsheet`-shaped - a testable
core module, an untested `*_io.cppm` talking to a real fd, a thin
`main.cpp` driver), built specifically to demonstrate `stop_token`/
`with_stop<T>()` end to end rather than only in unit tests. The player
is shown a random digit string and has to type it back before a
per-round deadline (`difficulty * length * base_unit`); a correct
answer grows either `difficulty` or `length` for the next round. Two
cancellation points, deliberately shaped to be genuinely different
rather than the same mechanism twice:

- The round's own timeout is cancelled *eagerly* via a token-aware
  `sleep_for()` the instant an answer arrives - real proof this isn't
  just "stop watching": `digit_recall_tests.cpp`'s own correct-answer
  test uses the identical fake-clock-never-advances idiom
  `loop_tests.cpp` uses for `loop::cancel_timer()` itself.
- A whole-session time budget can cut a round short via `with_stop()`
  even while the player is still mid-keystroke - honestly non-eager:
  the underlying read keeps running, unobserved, exactly matching that
  function's own documented limitation (also unit-tested: completing
  the abandoned read after the fact is proven a no-op).

Caught one real bug building it: `main()` initially left `loop.run()`
blocked on the session-length timer even after the game itself ended
(`quit`/a wrong or timed-out round) - `run()` only returns once nothing
is pending, and that timer was still sitting there with up to 90 real
seconds left. Fixed by having the session's own completion eagerly
cancel that timer and call `loop.stop()`, rather than waiting for
`run()` to drain everything on its own - caught by actually running the
program end to end (piped `quit`/wrong-answer/EOF/timeout input inside
the devenv container), not by the unit tests alone, which never
exercise `main()` itself.

**`future<T>`/`future_state<T>` gained `ready_with_value()`**, and
**`failed()` was renamed to `ready_with_failure()`** (both on
`future_state<T>` and `future<T>`) - a follow-up prompted by
`digit_recall.cppm`'s own `timeout.ready() && !timeout.failed()` check
after its `when_any()` race, which is exactly the pattern
`est::when_any()`'s own doc comment already describes as the intended
way to find out which future won a race, and won cleanly. `ready_with_
value()` collapses that combination into one call, computed the same
way `ready()`/`ready_with_failure()` already are - straight off
`result_`'s own variant index, no new state. `ready_with_failure()` is
a pure rename, not a new method: `failed()` predates this PR (Issue
#23) and was already used in `future.cppm`'s own dispatch logic,
`with_stop.cppm`, `when_any_succeeds.cppm`, and every test file that
exercises failure - renamed everywhere it appeared (source, tests, and
`docs/wiki/Continuation-Node-Mechanism.md`, which documents current
behavior) so the three queries read as a matched trio:
`ready()`/`ready_with_value()`/`ready_with_failure()`. Historical
`docs/PLAN.md` entries describing the original `failed()` addition
(Issue #23 and later) were deliberately left using that name - they
narrate what happened at the time, not the code as it reads today.

**`promise<T>::get_future()` was added**, and `detail::stop_state` was
removed as a result. `get_future()` is the producer-side mirror of
`future<T>::clone()` (`est/src/future.cppm`): given a `promise<T>`, it
returns a fresh `future<T>` aliasing the same `future_state<T>`, any
number of times, constrained to `T = void`/scalar `T` for the identical
moved-from-hazard reason `clone()` already is. This is the safe
direction - unlike a hypothetical `future<T>::get_promise()` (considered
earlier in this same PR's design discussion and rejected: it would let a
caller fabricate a second producer from an existing future, breaking the
structural single-producer guarantee `promise<T>`'s move-only-ness
exists to provide), `get_future()` only ever adds more consumers, the
same thing `clone()` already safely allows.

With `get_future()` available, `est::stop_source`/`est::stop_token`
(`est/src/sync/stop_token.cppm`) no longer need `detail::stop_state`, a
`promise<void>`+`future<void>` pair wrapped in its own separately-
allocated `shared_ptr<stop_state>` control block - that wrapper was pure
duplication, since `promise<T>`/`future<T>` are already thin
`est::shared_ptr<future_state<T>>` handles sharing one allocation
between them. `stop_source` now holds a bare `promise<void>`; `stop_token`
now holds a bare `future<void>`, derived from the promise on demand via
`get_future()` at `get_token()` time (and, internally, every time
`stop_source` itself needs to query `ready()` for `request_stop()`'s
idempotency guard or `stop_requested()` - each call constructs and
immediately discards a throwaway `future<void>`, a plain non-atomic
refcount bump/decrement, not a new allocation). This removes one heap
allocation from every `stop_source` construction (the now-gone
`stop_state` control block) and, as a side effect, fixes a minor
existing wart: `stop_source`'s own `allocator_type` constructor
parameter used to control only where that (now-removed) wrapper lived,
never the `future_state<void>` itself, which always resolved
`current_allocator()` internally regardless of what was passed in -
`stop_source`'s constructor now forwards `allocator` to
`detail::make_promise_future_impl<void>()` (`est/src/promise.cppm`)
directly, so it genuinely controls where the `future_state<void>` lives.

**A code review of this same change caught a real regression before it
shipped**: `stop_token`'s doc comment claims it's copyable, "matching
`std::stop_token`'s own copyable-handle shape" - true when it held a
`shared_ptr<detail::stop_state>` (implicitly copyable), but silently
false once its only member became a bare `future<void>`, since
`future<T>` itself deletes its copy constructor. The implicitly-declared
copy constructor a class gets from a non-copyable member is itself
deleted, so `stop_token` had quietly become move-only, contradicting its
own comment - no test caught it, since nothing in this codebase happened
to copy a `stop_token` object directly (every consumer calls
`get_token()` again instead). Fixed by giving `stop_token` an explicit
copy constructor/assignment built on `future<void>::clone()` (the same
tool `stopped()` already uses) - each copy gets its own `future<void>`
handle, all aliasing the same underlying `future_state<void>`.

The same review raised the question for `stop_source` too: it now holds
a bare `promise<void>`, and `promise<T>` is *also* move-only, so
`stop_source` silently lost copyability the identical way. Decided,
deliberately, **not** to restore it: `std::stop_source` genuinely is
copyable in the real standard (copies share one stop-state, part of
treating `stop_source`/`stop_token` symmetrically as cheap, shared
handles - P2175), but restoring that here would need `promise<void>`
wrapped in its own `shared_ptr` again, since `promise<T>`'s move-only-
ness is what keeps "at most one producer" structural rather than merely
documented - undoing the one allocation this entire change exists to
remove, for a property nothing in this codebase currently uses (no
caller copies a `stop_source`; every consumer shares via `stop_token`
instead). `stop_source`'s own doc comment now says so explicitly, so the
narrowing reads as an intentional, documented scope decision rather than
an accident matching `stop_token`'s.

Tests: two new cases in `est/tests/future_tests.cpp` for
`get_future()` itself (aliases the original before/after `set_value()`,
and repeated calls each an independent handle onto the same state); one
new case in `est/tests/stop_token_tests.cpp` proving `stop_token`
copy-construction and copy-assignment both observe the same
`request_stop()` as their original.
Full devenv pipeline re-verified (`cmake --preset default/ci/sanitize` +
`ctest` + `clang-format` + `clang-tidy` + the coverage gate).
`docs/wiki/Architecture.md`'s `:sync.stop_token` description updated to
match; `docs/PLAN.md`'s own entries above describing the original
`detail::stop_state` design were left as-is - they narrate what was
built at the time, not the code as it reads today.

---

### GitHub wiki sync (`.github/workflows/wiki.yml`)

With the GitHub wiki enabled for this repo, added a workflow that
mirrors `docs/wiki/*.md` into it (a wholly separate git repo,
`<repo>.wiki.git`) on every push to `main` that touches `docs/wiki/`,
plus `workflow_dispatch` for an on-demand run. Deliberately one-way and
additive-only: it copies/overwrites the wiki's same-named pages, never
deletes one `docs/wiki/` doesn't have, so the in-tree copy stays the
single source of truth (per this file's own top-level rule that
`docs/wiki/` "must stay in sync with the code as it changes") rather
than the wiki becoming a second, independently-editable copy that can
drift from it. Handles the wiki's git repo not existing yet (GitHub only
creates it lazily, on the first page ever saved) by falling back to
`git init` locally instead of failing the job, so the very first run
right after enabling the wiki still works without a manual placeholder
page first.

Extended the same workflow to also mirror this very file, `docs/PLAN.md`
itself, into the wiki as a page named `Journal` - it already *is* a
journal (this file's own top comment: "every milestone, review finding,
and bug fix, in the order it happened"), so publishing it alongside the
reader's-guide pages just makes that explicit rather than leaving it
reachable only via a repo-relative link. `docs/wiki/Home.md` gained one
added sentence pointing at it, deliberately *alongside* its existing
`[docs/PLAN.md](../PLAN.md)` link rather than replacing it: that relative
link is correct when `Home.md` is read in-repo (a real sibling-of-parent
path, `docs/wiki/Home.md` -> `docs/PLAN.md`) but would resolve to nothing
once mirrored into the wiki's flat page namespace, while a page-name link
(`Journal.md`, matching every other cross-page link already in that
file) is correct only in the wiki. Keeping both, rather than picking one
and breaking the other context, was cheaper than making every relative
link in `docs/wiki/` context-aware.

**The workflow's first real run, on merge to `main`, failed** - `git
clone .wiki.git` correctly hit the "wiki has never had a page saved"
case and fell back to a local `git init` as designed, but the later
`git push` to that same not-yet-existing remote failed the identical way
(`remote: Repository not found`), which the workflow hadn't accounted
for. Root cause, confirmed via the repo API (`has_wiki: true`) and a
direct clone probe: enabling the wiki feature alone doesn't provision its
underlying git repo - GitHub only does that the first time a page is
saved through the wiki's own web editor, and no git operation (push
included) can substitute for that one-time step. Fixed the workflow to
say so plainly - both in its own top comment (previously, incorrectly,
implying `workflow_dispatch` alone could bootstrap an empty wiki) and as
an `::error::` with the exact URL and next step if the push fails - rather
than leaving a future occurrence to a bare, cryptic git exit code. The
manual step itself (saving one placeholder page via the web UI) is still
outstanding and has to happen before this workflow can succeed for real.

---

### Issue #31: priority levels for `est::loop`'s ready-queue

Design discussion happened directly on the issue (four levels
- `est::Priority::background`/`normal`/`high`/`critical` - a plain
parameter on `then()`/`then_fast()` defaulting to inherit `current_priority()`
rather than anything wired into a specific primitive like
`est::external_event<T>`, and a pluggable scheduler rather than a fixed
drain order). Implemented the self-contained core: priority levels, the
`ready_node` field, the scheduler abstraction with an eager default, and
`then()`/`then_fast()`'s inheritance via an ambient `current_priority()`/
`set_priority()` - deliberately leaving the proportionate (fairness)
scheduler, starvation detection (`platform::interface` additions mirroring
the existing stall detector), and `spawn()`'s own priority parameter (issue
#58, itself not implemented yet) as documented follow-ups rather than
folding an already-large design into one PR.

**`est::Priority`** (`est/src/loop.cppm`) - four levels, declared (and
exported) directly in `:loop` rather than alongside
`current_loop()`/`current_allocator()` in `:util.current_loop`: that
partition already imports `:loop`, so `:loop` importing back would be
circular, the identical constraint that already keeps
`detail::current_allocator_new_delete<T>` a separate mixin there instead of
living directly on `ready_node`/`timer_node`. `detail::ready_node` gains a
plain public `priority_level` field (defaulting to `Priority::normal`,
matching `intrusive_list_node::next`'s own already-public, no-accessor
style - there's no invariant here to protect).

**`est::current_priority()`/`est::set_priority()`** - a `thread_local`
ambient value plus an RAII guard, modeled on `make_current_loop()`'s own
shape (built on `est::scope_exit` the same way) but a plain save/restore
stack rather than a single slot with a checked precondition against
nesting: nested priority scopes are the expected, common case here (a
background chain lowering it inside a caller that already raised it), not
a programming error the way two loops both being "current" is.

**Inheritance, not just explicit setting**: `future_state<T>::then()`/
`then_fast()` (and `future<T>`'s four forwarding overloads) gained a
trailing `Priority prio = current_priority()` parameter - the default
argument is evaluated at the `then()`/`then_fast()` call site itself, so
omitting it means "inherit whatever's ambient right now," while an
explicit value overrides it. `loop::run_one()` sets `current_priority()` to
the currently-running node's own `priority_level` for the whole duration of
its `run()` (mirrored in `future_state<T>::set_continuation()`'s own
inline-run path for `then_fast()`, for consistency) - this is what makes a
*chain* inherit automatically: anything a running continuation goes on to
register picks up its priority by default, without threading a parameter
through every intermediate call.
`future_awaiter<T>::await_suspend()` stamps its own resume node's
`priority_level` from `current_priority()` explicitly, the same way, since
`co_await`'s own syntax has no room for an extra argument the way
`then()`/`then_fast()` do.

**The scheduler**: `loop::ready_` became a `ready_queues` -
`std::array<intrusive_list<detail::ready_node>, 4>`, one FIFO per level -
drained by a pluggable `loop::scheduler_type`, set once at construction
(defaulting to `loop::eager_scheduler`, which always drains the highest
non-empty level first). `std::function`, not `std::move_only_function` as
the issue itself proposed: the pinned Clang 22 libc++ snapshot this
codebase builds against doesn't implement `std::move_only_function` yet
(`__cpp_lib_move_only_function` is undefined) - confirmed by a failed build
before switching. `std::function`'s copy-constructible-callable requirement
costs nothing in practice here (the eager scheduler is a stateless function
pointer; a future fairness-oriented one only needs plain, copyable
per-level counters), so this is a pragmatic substitute, not a design
change - worth revisiting once libc++ catches up. Deliberately type-erased
rather than a second `platform::interface`-style virtual base: a scheduler
decision happens on the hottest path this codebase has, once per ready
node, so it stays the same kind of type erasure
`std::pmr::polymorphic_allocator` already is throughout this codebase
(`est::shared_ptr` et al.), not a second inheritance hierarchy alongside
`platform::interface` - `CLAUDE.md`'s own "platform is the only
runtime-polymorphic seam" claim stays true.

**Explicitly out of scope, documented on the issue and in
`docs/wiki/Loop-And-Timers.md`**: the proportionate/fairness scheduler
(eager can starve a lower level indefinitely under sustained higher-
priority load - accepted for now); starvation *detection* itself (proposed
to mirror `reset_loop_stall_detection()`/`detect_loop_stall()` exactly, via
new `platform::interface` methods taking each level's head pointer);
`spawn()`'s own priority parameter (issue #58, not implemented); and the
monadic-flatten path (`detail::flatten_forwarder<T>`), which still always
runs at `Priority::normal` rather than inheriting - not touched in this
pass.

**Tests** (`est/tests/loop_tests.cpp`): higher-priority ready work drains
before lower-priority work regardless of enqueue order; `then()`/
`then_fast()` default to inheriting `current_priority()` at the call site;
an explicit priority argument overrides that inheritance; `set_priority()`
nests and restores correctly, like a stack; a continuation registered from
inside a running node inherits *that node's* priority, not whatever was
ambient before it started. All 300/300 existing tests pass unchanged - the
default-everything-`normal` behavior is exactly the prior single-queue
FIFO ordering. Full devenv pipeline (`cmake --preset default/ci/sanitize` +
`ctest` + `clang-format` + `clang-tidy` + the coverage gate - 100% diff
coverage) plus the separate `wasm32` project build, all re-verified.
`docs/wiki/Loop-And-Timers.md`'s "The ready-queue" section updated (code
samples were now stale) and extended with a new "Priority levels and the
pluggable scheduler" subsection; `docs/wiki/Home.md`'s lookup table
updated to match.

**A code review of the same PR caught a real gap** before it shipped:
`est::yield_execution()` (`est/src/promise.cppm`) and
`counting_event<Mode>::wait()`'s slow path (`est/src/sync/event.cppm` -
which `est::mutex::lock()` is built directly on) each construct their own
`detail::promise_resume_node<void>` and hand it to `loop::enqueue_ready()`
(directly, or later via `set()`) without ever stamping `priority_level`
from `current_priority()` - both silently defaulted to `Priority::normal`
regardless of the caller's actual ambient priority, since neither goes
through `future_awaiter<T>::await_suspend()` (the usual place a `co_await`
inherits it). Unlike the flatten/monadic path (`detail::
flatten_forwarder<T>`, `spawn()`) - explicitly documented as staying at
`Priority::normal` for now - this one wasn't a deliberate exclusion,
just an oversight: a coroutine running at `Priority::critical` that then
`co_await`s `yield_execution()` or a contended `est::mutex::lock()` would
silently drop to normal priority for its resumption, exactly the kind of
priority inversion the whole inheritance mechanism exists to prevent.
Fixed by stamping `priority_level = current_priority()` at both
construction sites. Two new regression tests (`est/tests/loop_tests.cpp`,
`est/tests/event_tests.cpp`) catch it directly: a marker at
`Priority::normal` is enqueued to `loop`'s ready-queue *before* the
`yield_execution()`/`wait()` call under test, so if either incorrectly
defaulted to normal too, FIFO ordering within that shared level would run
the marker first, before the future under test is even ready - both tests
assert it isn't. All 302 tests pass (300 + these 2); full devenv pipeline
re-verified.

### Issue #110: the flatten/monadic path didn't inherit `Priority` either

The third bypass path, found in a post-merge follow-up pass over #31's
priority mechanism (not the code review that caught the two above -
`docs/PLAN.md`'s own "Issue #31" entry above already named this one
explicitly as a known, deliberate exclusion, "staying at `Priority::normal`
for now" - but never actually filed a tracking issue for it, and it turned
out to be worth fixing rather than leaving open). `future_state<T>::
then_impl()`'s inner `fulfill()` (`est/src/future.cppm`) constructs a
`detail::flatten_forwarder<U>` node whenever a `.then()` callback itself
returns a `future<U>` (automatic flattening instead of a nested
`future<future<U>>`) - and, like `yield_execution()`/`counting_event::
wait()` before the earlier fix, never touched that node's `priority_level`,
silently defaulting to `Priority::normal` regardless of the outer chain's
own priority. A chain built entirely at `Priority::critical` that happened
to flatten a nested future dropped to normal for that one hop, and
whatever ran after it inherited normal too (`run_one()`'s ambient-priority
guard reads whichever node is actually running).

Fixed identically to the earlier two: `node->priority_level =
current_priority();` right after construction, before `set_continuation()`
- `fulfill()` runs synchronously inside whichever node's `run()` invoked
the outer `.then()` callback, and `run_one()` has already set
`current_priority()` to that node's own priority for the whole call, so
reading it back is correct (the same reasoning the `yield_execution()` fix
documents).

**The test gap, checked explicitly before writing the regression test**:
existing coverage tested the flatten path (correctness, move-not-copy,
failure propagation, void flattening, leak-freedom) and the priority
mechanism (inheritance, override, nesting, chain-inherits-running-node's-
priority) thoroughly, but never in combination - no test passed a
`Priority` argument through a `.then()` callback that returns a future.
New test (`est/tests/loop_tests.cpp`) uses the same discriminating pattern
as the earlier fix: a `Priority::normal` marker enqueued before an
already-ready outer `future<int>` whose `.then(fn, Priority::critical)`
callback returns another already-ready `future<int>` (triggering the
flatten branch); the marker's own callback checks whether the flattened
result is already ready by the time it runs. Verified the test actually
discriminates before trusting it: temporarily reverted the fix and
confirmed the test fails (`REQUIRE( flattened_ready_when_marker_ran )`
false) before restoring the fix and confirming it passes. All 303 tests
pass (302 + this one); full devenv pipeline (format, `ci` build+test,
`clang-tidy`, 100% new-code coverage, `sanitize` build+test, wasm32 build)
re-verified. Also filed two further #31 follow-ups that had no tracking
issue at all - a proportionate/fairness scheduler (#108) and starvation
detection mirroring the existing stall-detector (#109) - and added the
missing `spawn()` priority-parameter scope note to #58, closing every gap
between #31's original design discussion and what actually has a tracking
issue today.

## Issue #57: `est::with_timeout<T>()`

Implements the issue's own ask - bound how long a `future<T>` is allowed
to take - now that both of its named dependencies (#54's `when_any()`/
`when_all()`, #56's cancellation machinery) exist. The issue's original
proposed composition, "essentially `when_any(operation, sleep_for(timeout))`",
was speculative from before either primitive was built; having both now
made it possible to see a real gap in that composition rather than just
implement it as first proposed: neither `when_any()` nor `with_stop<T>()`/
`stop_token` has a way to reach into the timer it would be racing against
to cancel it, so building `with_timeout<T>()` on top of them would leave a
fired-but-unused deadline timer sitting in `loop`'s timer queue for its
full duration even after `operation` already won.

Built instead as its own racer (`est/src/with_timeout.cppm`, new partition
`est:with_timeout`), directly on `:promise`'s own pieces
(`detail::sleep_resume_node`, `loop::schedule_timer()`/`cancel_timer()`) -
exactly the shape `est:with_stop`'s own `sleep_until(deadline, const
stop_token&)` already established, for the identical reason (that overload
isn't built on `with_stop<T>()` either, same doc-comment reasoning). When
`operation` wins the race, the now-redundant deadline timer is reclaimed
immediately via `loop::cancel_timer()` rather than left to fire uselessly
later - genuinely eager cancellation, not just "stop watching." When the
deadline wins, the returned future fails with a new, distinct exception,
`est::operation_timed_out` - deliberately not a reuse of `operation_cancelled`
(`est:sync.stop_token`): `with_timeout<T>()` has no `stop_token` of its
own, so a caller catching `operation_cancelled` elsewhere to mean
"something explicitly requested cancellation" shouldn't also have to catch
it for "this simply took too long," a different condition.

Same honestly-scoped limitation as `with_stop<T>()`/`when_any()`/
`when_all()`/`when_any_succeeds()`: losing the race only stops the
*caller* from waiting on `operation` further, it keeps running in the
background until it completes on its own (the eventual completion reaches
an already-`done`-guarded racer and is silently dropped).

Five new tests (`est/tests/with_timeout_tests.cpp`, matching
`with_stop_tests.cpp`'s own shape): normal completion forwards value/
failure unchanged; the deadline firing resolves `operation_timed_out` and
the operation's later completion is a no-op; `with_timeout<void>()`'s
success path. The eager-cancellation test mirrors `loop_tests.cpp`'s own
`sleep_for(delay, stop_token)` cancellation test exactly - a deliberately
huge timeout (1000s) plus asserting the fake clock never had to advance
(so `run_until_idle()` never called `platform::instance().sleep_until()`
toward that deadline) proves the timer was actually pulled out early, not
merely left to fire later; combined with an allocation-balance check via
`counting_resource`. Verified this test actually discriminates before
trusting it, same as issue #110's own regression test: temporarily removed
the `cancel_timer()` call and confirmed the test fails (clock advances to
`1000s`, the assertion catches it) before restoring the fix and confirming
it passes.

**A sixth test, attempted and dropped - a real finding, not a test bug.**
Tried to cover the `tf.ready_with_failure()` branch (the deadline timer
itself getting abandoned rather than firing normally) by tearing the loop
down while both racers were still pending. It never worked, and tracing
through why turned up something genuine: `loop::drain_pending()`
(`est/src/loop.cppm`) abandons `pending_timers_` first, then - in the
*same* call - abandons whatever's sitting in `ready_`, including any node
that timer abandonment itself just enqueued there via the completed
timer's own `waiters_` drain. A `then_fast()`-registered racer reaches its
callback through `run()`, but a node still sitting in `ready_` when
`drain_pending()` reaches its second phase gets `abandon()` instead -
which `concrete_continuation<Fn, U>::abandon()` (`est/src/future.cppm`)
implements by completing its own *discarded* downstream future directly,
without ever invoking `fn_`. So the callback that would forward the
abandonment into `with_timeout<T>()`'s own `state->result` never runs, and
the future `with_timeout<T>()` returned is left permanently pending - not
completed with a timeout, not with an exception, just stuck. Confirmed via
`~loop()` too (`~loop() { drain_pending(); }` - identical path). This
isn't specific to `with_timeout<T>()`: `with_stop.cppm`'s own
`sleep_until(deadline, const stop_token&)` has the structurally identical
`tf.ready_with_failure()` branch, reached the same way, and by the same
reasoning is equally unreachable there - a pre-existing, latent gap in the
`then_fast()`-pair-of-racers combinator shape itself, not something this
change introduced. Left the branch in place (documented in a comment
explaining exactly why it's currently unreachable, mirroring
`with_stop.cppm`'s own shape) rather than deleting it as dead code, since
`drain_pending()`'s single-pass behavior isn't necessarily permanent (#103's
registry rearchitecture could change it) - but didn't try to *fix* the
underlying stuck-future gap here, since it's shared by already-shipped code
and deserves its own deliberate look, not a fix folded into an unrelated
issue. Diff coverage lands at 85% (comfortably past the 80% gate) rather
than the 100% recent PRs have hit, entirely on this one now-understood,
documented gap plus unexercised `platform::interface`/`memory_resource`
stub overrides the test file's own fixtures must implement but don't all
need.

All 308 tests pass (303 + these 5); full devenv pipeline (format, `ci`
build+test, `clang-tidy`, 85% new-code coverage, `sanitize` build+test,
wasm32 build - the wasm32 project needed its own separate `CMakeLists.txt`
updated with the new partition too, caught by that build failing) re-
verified. `docs/wiki/Coroutines.md`'s "Cancellation: `stop_token` vs.
abandonment" section gets a new subsection; `Home.md`'s lookup table
updated to match.

**Code review pass (PR #112), two findings, both fixed:**

1. The stuck-future gap above was real but wasn't disclosed on
   `with_timeout<T>()`'s own public doc comment - a caller reading only the
   exported API had no way to know about it. Filed as its own issue (#113 -
   full trace-through, proposed directions, cross-references both affected
   `with_stop.cppm` call sites) rather than left as just a PR-description
   aside, and the doc comment now names it explicitly and points at #113.
2. `new detail::sleep_resume_node(...)` immediately followed by
   `schedule_timer()` (which does its own, non-`noexcept` allocation -
   `pending_timers_.reserve()`, `timer_queue::schedule_at()`) wasn't
   exception-safe: a `bad_alloc` from `schedule_timer()` would leak the
   just-allocated node, since nothing referenced it yet. Reproduces an
   identical pre-existing ordering in `with_stop.cppm`'s own token-aware
   `sleep_until()` and `promise.cppm`'s plain `sleep_until()` - not
   introduced here, but now existing at a third call site was reason enough
   to fix this one directly rather than let it spread further unaddressed.
   Fixed with a `std::unique_ptr` guard, released only after
   `schedule_timer()` returns successfully - ownership transfers to
   `loop`'s own `pending_timers_` at that point, matching the codebase's
   existing "guard until handed off" idiom (`run_one()`'s own
   `unique_ptr<ready_node>` guard, `est:loop`). The other two call sites
   weren't touched (out of scope for this PR - a pre-existing pattern
   shared by already-shipped code, same reasoning as #113 above).

Diff coverage moved to 88% with the doc-comment/guard additions (still the
same one documented, currently-unreachable branch as the only real gap).
Full pipeline re-verified again after these fixes: 308/308 (`ci`), clean
`clang-tidy`, `sanitize` 251/251, wasm32 build clean.

---

### Allocation simplification for `with_timeout<T>()`/`with_stop.cppm`'s `sleep_until(deadline, stop_token)`, and a real fix for issue #113

Delegated the question "does `with_timeout<T>()` allocate more than it
needs to?" to a research subagent before touching any code. Its finding:
`with_timeout<T>()` and `with_stop.cppm`'s own `sleep_until(deadline, const
stop_token&)` (the shape it deliberately mirrors) both did 6 allocations
per call - not because `with_timeout<T>()` was doing anything wasteful its
sibling avoided, but because both built the deadline-timer racer the same
way: `detail::sleep_resume_node` (the generic node behind the plain,
non-racing `sleep_until()`/`sleep_for()` in `promise.cppm`) completes a
`promise<void>`, which needed its own `future_state<void>` allocation plus
a `.then_fast()` continuation node just to bridge that result back out
into the combinator's own shared racer state. `sleep_resume_node` was the
natural thing to reach for - already built, already tested, and it kept
every racer in these combinators looking identical (a plain `future<T>` +
a `.then_fast()` lambda), rather than special-casing the timer side as a
bespoke node. The cost of that uniformity: two allocations spent on a
round-trip that was otherwise pure overhead.

Fix (applied to both `est/src/with_timeout.cppm` and `est/src/with_stop.cppm`,
kept mirrored per their own doc comments): a dedicated timer-node type per
combinator (`detail::with_timeout_timer_node<T>`,
`detail::sleep_stop_timer_node`) holding a `shared_ptr` to the combinator's
own racer state directly, with `fire()`/`abandon()` writing straight into
`state->result` instead of completing a second future. 6 allocations per
call down to 4 (the result `future<T>`'s own state, the racer state, the
timer node, and one remaining `.then_fast()` node for the operation/token
racer - the timer racer no longer needs one).

A second, more consequential effect fell out of removing the bridge: issue
#113's stuck-future gap - `loop::drain_pending()`'s two-phase abandonment
(pending timers first, then whatever that cascades into `ready_`) could
abandon-instead-of-run a still-queued `.then_fast()` continuation, silently
dropping the bridge future's own completion and leaving the combinator's
returned future permanently pending if the loop tore down while both
racers were still in flight. With the bridge gone, a still-pending timer
node's `abandon()` completes `state->result` directly, in
`drain_pending()`'s *first* phase - there's no intermediate `ready_` node
left for the second phase to lose. This closes the gap for both
`with_timeout<T>()` and `with_stop.cppm`'s `sleep_until(deadline, const
stop_token&)`, not just documents around it. `done` is checked and set
inside `fire()`/`abandon()` themselves now (not only inside the surviving
racer's own `.then_fast()` callback), since `loop::cancel_timer()` calls
`abandon()` synchronously - the operation/token racer winning and calling
`cancel_timer()` on an already-fired timer needs the same guard on the
same call stack.

Two new regression tests prove the fix rather than just asserting it:
`est/tests/with_timeout_tests.cpp` and `est/tests/loop_tests.cpp` each gained
a "loop teardown while both racers are still pending" case that calls
`loop.drain_pending()` directly (what `make_current_loop()`'s own guard and
`~loop()` both call) with the operation/token never having fired and the
deadline timer still pending, then asserts the returned future is
`ready_with_failure()` instead of checking nothing (which is what the
identical scenario used to do before this fix - permanently hang, the
exact shape of the earlier, deliberately-removed sixth `with_timeout` test
from the previous entry above, except this time it actually passes).

Full pipeline re-verified: 310/310 (`default`), clean `clang-format`, clean
build + clean `clang-tidy` + 93% diff coverage (`ci`), `sanitize` 253/253,
wasm32 build + Node smoke test clean.

---

### Issue #114: convert naked `new`/`delete` to `unique_ptr`

Filed right after the with_timeout/with_stop allocation-simplification pass
above found (and fixed) two of these; this issue asked for the same
treatment across the rest of `est/src`. A full audit found 8 remaining
naked `new` sites, all following the same shape: `auto* node = new
T(args); loop_ref.enqueue_ready(*node)/schedule_timer(*node, ...);` - the
node's true ownership transfers implicitly to whichever of `est::loop`'s
containers takes the raw pointer, with nothing but convention holding that
together in between.

- `est/src/promise.cppm`: `sleep_until()` (calls `schedule_timer()`, not
  `noexcept` - a real leak-on-`bad_alloc` site, same class as the ones
  already fixed in `with_timeout.cppm`/`with_stop.cppm`), `yield_execution()`
  (calls `enqueue_ready()`, `noexcept` - no actual leak risk today, but
  still a naked `new`).
- `est/src/sync/event.cppm`: `counting_event<Mode>::wait()`'s slow path
  (`waiters_.enqueue()`, `noexcept`).
- `est/src/future.cppm`: `future_state<T>::then_impl()` (registers a
  `concrete_continuation<Fn, U>` via `set_continuation()`), `fulfill()`'s
  flatten branch (registers a `detail::flatten_forwarder<U>`, same call),
  `future_awaiter<T>::await_suspend()` (registers a `future_resume_node<T>`,
  same call) - `set_continuation()` isn't marked `noexcept`, but in
  practice only reaches `waiters_.enqueue()`/`loop::enqueue_ready()`
  (both `noexcept`) or a guaranteed-safe `shared_from_this()`.
- `est/src/timer_periodic.cppm`: `periodic_timer_node<Fn>::fire()`'s own
  re-arming `new` and `schedule_periodic()`'s initial one - both call
  `schedule_timer()`, both real leak-on-`bad_alloc` sites.

Converted every site to `std::make_unique<T>(args...)` immediately followed
by `.release()` right after the handoff call succeeds - the identical
guard-then-release idiom the with_timeout/with_stop fix above already
established, generalized to every remaining site regardless of whether
today's handoff happens to be `noexcept` (uniform treatment reads more
consistently than special-casing the two that don't strictly need it, and
costs nothing - `release()` after a `noexcept` call is just as safe as
after a throwing one). Also converted the two already-guarded sites in
`with_timeout.cppm`/`with_stop.cppm` from `std::unique_ptr<T> guard(new
T(args))` to `std::make_unique<T>(args)` for the same reason - both forms
were already accepted by the issue's own wording, but `make_unique`
reads slightly cleaner and leaves zero remaining `new` expressions for an
owning pointer anywhere in the tree. `est::shared_ptr<T>::make()` was
never in scope for this - it already goes through
`std::pmr::polymorphic_allocator::new_object()`, never a raw `new`
expression, so there was nothing to convert there.

A repo-wide grep after the fact (`est/src`, `examples`, `estwasm`,
`estext`) confirms zero remaining naked `new` expressions for an owning
pointer, and zero naked `delete` statements (every deletion already went
through the `std::unique_ptr<Node>(&node)`-then-implicit-destruction
idiom `est::loop`'s own `run_one()`/`fire_ready_timers()`/
`abandon_ready_node()`/`abandon_timer_node()` established).

Full pipeline re-verified: 310/310 (`default`), clean `clang-format`, clean
build + clean `clang-tidy` + 95% diff coverage (`ci`), `sanitize` 253/253,
wasm32 build + Node smoke test clean.

---

### Issue #117: verify `future<T>`/`promise<T>` for a move-only `T`

"Verify that a future of a move only type works or require some special
care." It works, end to end - `set_value(std::move(...))`/
`std::move(future).get()`, `co_await`, `then()`/`then_fast()`, and the
flatten path (already had one regression test of its own,
`future_tests.cpp`'s "flattening a then() that returns a
future<unique_ptr<T>> moves, not copies, the value") - confirmed with 7 new
`std::unique_ptr<int>`-based test cases added to `future_tests.cpp` rather
than just reasoned about from the source, since this is exactly the kind
of template/SFINAE question worth actually compiling.

One real constraint, matching the issue's own guess ("the continuation
taking the param only by ref"): a `.then()`/`.then_fast()` callback
registered on a `future<T>` for a move-only `T` must take the value by
reference (`const T&`, or a generic `auto&`/`auto&&`), never by plain
value. `concrete_continuation<Fn, U>::run()` (`future.cppm`) compiles both
of its `owner_.count() == 1` runtime branches unconditionally whenever `Fn`
is invocable with `T&&` - the move-out path and the reference-read path
alike, since which one actually executes is a runtime decision, not a
compile-time one - and the reference-read path would need to
copy-construct a by-value `Fn`'s parameter, which a move-only `T` can't
do.

That rejection does happen cleanly, at the `.then()`/`.then_fast()` call
site itself: `then_callback_for<Fn, T>`'s own `std::invocable<Fn&, const
T&>` check is already false for a by-value `Fn` on a move-only `T`
(selecting a deleted copy constructor inside an unevaluated
`requires`-expression's checked call makes the requirement simply
unsatisfied, not a hard error - genuinely SFINAE-friendly). Tried to pin
this down with a `static_assert(!requires(...))` test, the same idiom
`spsc_ring_tests.cpp` already uses for its own move-only-vs-copyable
checks - a real, if minor, finding: it doesn't work here, because
`future<T>::then()`/`then_fast()` (the outer wrappers a caller actually
calls) declare a deduced `auto` return type, so determining it means
instantiating their *body*, and a body-instantiation failure is a hard
compiler error, not a substitution failure, even from inside an
otherwise-unevaluated `requires`-expression - discovered by writing the
static_assert, watching the whole file fail to compile with a real
"no matching member function" hard error instead of the clean
"constraints not satisfied" one, and tracing through why. Removed that
test; documented the finding in prose instead (`future_tests.cpp`'s own
section comment, and `docs/wiki/Continuation-Node-Mechanism.md`'s new
"Move-only `T`" subsection, which ties it to the identical
deleted-function/body-instantiation principle that section already
documents for wrapped-mode callbacks).

Full pipeline verified: 317/317 (`default`), clean `clang-format`, clean
build + clean `clang-tidy` + 100% diff coverage (`ci`), `sanitize`
260/260, wasm32 build + Node smoke test clean (no `est/src` changes this
time - test-only).

---

### Issue #116: `future<T>::take()`/`est::extraction<T>` - a real, compile-time single-extraction guarantee

Investigated three alternatives (posted as issue comments before
implementing anything, per this repo's own "design first, comment on the
issue, then build" convention): reverting `get()` to classic ref-qualified
overloads plus Clang's `-Wconsumed` consumed-analysis attributes; fully
embracing the issue's own "safe by default, ref-count decides" proposal
with a debug-only `check()` (matching `future_state<T>::check_not_completed()`'s
own precedent) as the realistic safety net for that direction; and a
third, additive option - a small, dedicated proxy type carrying the real
compile-time guarantee, alongside `get()` left untouched. Chose the third:
lowest risk, doesn't touch anything #117/PR #118 just finished
documenting, and (verified directly, not assumed) needs zero
special-casing for move-only `T`.

**`est::extraction<T>`** (new, `est/src/future.cppm`): a small, move-only,
`[[clang::consumable(unconsumed)]]`-annotated wrapper around a value
already produced by `get()`. `value() &&` is `[[clang::callable_when(unconsumed),
clang::set_typestate(consumed)]]` - calling it twice, or on an
already-moved-from `extraction<T>`, is now a compile error under
`-Wconsumed`, not a silent logic bug. Deliberately *not* deducing-this
(the one method in `est:future` that isn't): confirmed empirically during
issue #116's own investigation that Clang's consumed-analysis attributes
don't track state through an explicit object parameter at all, and
`extraction<T>` is small and self-contained enough that giving up
deducing-this for this one class costs nothing elsewhere.

**`future<T>::take() &&`** (new): pure sugar over `std::move(*this).get()`,
wrapping the result in `extraction<T>` - inherits every existing `get()`
guarantee (move-vs-copy dispatch, move-only `T` support) unchanged.
Constrained off `T = void` (`requires(!std::is_void_v<T>)`) - nothing to
guard a double extraction of for a `future<void>`.

**`-Wconsumed` enabled repo-wide** (`cmake/CompilerWarnings.cmake`, under
`-Werror` like every other warning here) - narrow in practice, since
`extraction<T>` is the only `[[clang::consumable]]`-annotated type in the
codebase, so it can only ever fire against that one type's own callers.
Verified the whole `ci`/`sanitize`/wasm32 pipeline stays clean with it on.

Two real implementation gotchas, both found by writing the tests and
tracing through the actual compiler diagnostics rather than assuming the
mechanism would just work, matching this session's own established
discipline:
- A single, genuinely correct `std::move(extraction_var).value()` call
  produces a `-Wconsumed` false positive when written directly inside a
  Catch2 `REQUIRE(...)`/`CHECK(...)` argument, for a *named*
  `extraction<T>` variable - Catch2's own expression-decomposition macros
  appear to reference a named sub-expression's type more than once
  (template/decltype machinery for printing the LHS/RHS on failure),
  confusing this syntactic analysis even though the call only executes
  once at runtime. Worked around by extracting via `.value()` in its own
  plain statement first, then `REQUIRE()`-ing the already-extracted
  result - documented directly in `future_tests.cpp`'s own section
  comment so the next person adding a test here doesn't have to
  rediscover it.
- `static_assert(!requires(est::future<void>&& f) { std::move(f).take(); })`
  (attempted, to guard `future<void>::take()` not existing) doesn't work
  either, for a *third*, distinct reason from the `then()`/`then_fast()`
  deduced-return-type case #117/PR #118 already found: verified directly
  that a `requires`-clause on an *ordinary* (non-template) member function
  of a class template isn't SFINAE-friendly inside a `requires(){...}`
  expression on this toolchain, even with an explicit (non-deduced)
  return type - a minimal, unrelated reproduction hard-errors with
  "invalid reference to function... constraints not satisfied" instead of
  making the requires-expression false. Removed the test; documented the
  finding in prose instead (matching how #117/PR #118 handled the
  identical-outcome, different-mechanism case).

Verified the actual enforcement works end to end, not just in isolated
scratch probes: temporarily reintroduced a deliberate double-`.value()`
call into `future_tests.cpp`, confirmed it fails to compile with exactly
the expected `-Wconsumed` diagnostic, then reverted it before committing.

Full pipeline verified: 320/320 (`default`), clean `clang-format`, clean
build + clean `clang-tidy` + 100% diff coverage (`ci`), `sanitize`
263/263, wasm32 build + Node smoke test clean.

---

### Issue #116 revisited: `take()`/`extraction<T>` reverted (PR #119)

Merged and closed in the entry above, then reconsidered and reverted
(clean `git revert` of PR #119's squash-merge commit, `est/src/future.cppm`
and the rest of that diff back out entirely).

Re-reading the issue's actual text against what shipped exposed a real
mismatch. The issue asked for `get()` itself to stop trusting the caller's
syntactic choice (`f.get()` vs. `std::move(f).get()`) and instead decide
move-vs-copy from the *live ref count of the future_state* - the same
`owner_.count() == 1` check `concrete_continuation<Fn, U>::run()` already
does internally for `then()`/`then_fast()` - with a `-Wconsumed`-style
annotation proposed only as the safety net for the one case a
refcount-based `get()` still can't make safe by construction (sole owner,
called twice).

What got built instead left `get()` completely untouched and added a
parallel, opt-in `take()`/`extraction<T>` that only guards against calling
`.value()` twice on a value `get()` had *already* extracted. That's a real
but different guarantee, and it doesn't touch the hazard the issue actually
named: `std::move(f).get()` racing a still-live clone of the same
future_state, silently corrupting the clone's view - a refcount-aware
`get()` would fix this structurally; `take()` does nothing for it. Worse,
`take()`'s real audience turned out to be narrow: `then()`/`then_fast()`
already get the count()==1 dispatch for free, and `co_await` structurally
consumes exactly once, so the only callers who'd ever reach for `take()`
are manual, non-coroutine code holding a `future<T>` handle directly - a
much smaller slice of the problem than "make `get()` safe by default." A
new type, a new repo-wide `-Werror` warning flag, and `-Wconsumed`'s own
two documented false-positive gotchas were a lot of surface for a guarantee
that's adjacent to, not a fulfillment of, what #116 asked for.

Issue #116 reopened; a refcount-driven `get()` redesign (or a decision to
close it as not worth the complexity, given the tension between "safe by
default" and static double-consumption detection already surfaced during
the `take()` investigation) is still open for a future pass.

---

### Issue #116 closed: investigated the wrapped-mode call sites directly, decided against a `get()` redesign

Before deciding whether a refcount-driven `get()` redesign is worth
building, traced through the actual codebase rather than reasoning
abstractly: every real place a `then()`/`then_fast()` continuation
receives a wrapped `future<T>&` and extracts a value from it (not just
`ready_with_failure()`/`get_exception()`).

First, clarified what a wrapped-mode callback's signature can even be:
`then_callback_for`'s wrapped half is exactly `std::invocable<Fn&,
future<T>&>`, and `concrete_continuation<Fn, U>::run()` always passes a
named local (`future<T> view(state.shared_from_this())`), never a
temporary - so only `future<T>&` or `const future<T>&` work (both bind to
that lvalue); `future<T>` by value hard-errors (copy ctor deleted) and
`future<T>&&` simply doesn't bind. Documented directly on
`then_callback_for` (`est/src/future.cppm`) - this wasn't spelled out
anywhere before.

Then grepped every wrapped-mode callback in `est/src` for one that
actually calls `.get()`/`std::move(...).get()` on the wrapped future
(as opposed to just inspecting `ready_with_failure()`). Exactly two:
`with_stop()` (`with_stop.cppm:70`, now shifted a few lines by the doc
comment below) and `with_timeout()` (`with_timeout.cppm:189`) - both
`then()`/`then_fast()`-based combinators taking `T` by value and
`std::move`-ing it into a single `then_fast()` registration, then
unconditionally `std::move(op).get()`-ing inside. `when_all`/`when_any`/
`when_any_succeeds`'s own wrapped-mode stages never call `.get()` at all.

Both of those two are safe today, but by a structural argument, not a
runtime check: `future<T>::clone()` - the only way to get a second live
handle onto the same `future_state` - is constrained to `void`/scalar `T`
(`future.cppm`), so a non-scalar `T` can't have been cloned, and neither
combinator registers more than the one continuation. The gap: that's a
caller-enforced invariant, not a compiler-checked one, and neither file
documented it - nothing stops (or even warns about) a caller who
independently attaches their own continuation to the same `future<T>`
before handing it to `with_stop()`/`with_timeout()` by value, which would
race that continuation against the combinator's own move. Fixed the cheap
part: added a doc comment at both `.get()` call sites explaining exactly
why the move is safe and what a caller must not do, mirroring
`future<T>::clone()`'s own doc comment's style of reasoning.

Decided against the redesign itself. The unwrapped path's
`owner_.count() == 1` check works because it happens *before* any
`future<T>` handle exists - at that point `owner_` really is the only
reference anywhere. That doesn't port cleanly to the wrapped path: by the
time a wrapped callback can call `get()`, there are structurally at least
two live references (the node's own `owner_`, still held until `run()`
returns, plus the `view`/`op` handle itself), so a naive `state_.count()
== 1` check inside `get()` would see 2 in the exact case that's actually
safe today and silently fall back to copying - regressing `with_stop()`/
`with_timeout()` from a free move to a guaranteed copy. Making this work
for real needs `get()` (or whatever it delegates to) to know how to
discount its own transient references, which is real design work with no
existing call site asking for it - the only two consumers that matter
today already get correctness for free from the structural argument
above, and the narrow gap that argument leaves (a caller double-attaching
a continuation) doesn't occur anywhere in this codebase.

Closed as "investigated, not pursuing a redesign," with the doc-comment
fix landing in its place. Full pipeline re-verified after the doc
additions: 317/317 (`default`), clean `clang-format`/`clang-tidy`,
`sanitize` 260/260, wasm32 build + smoke test clean (doc-only changes to
`.cppm` files, so no coverage-gate-relevant new lines).

---

### Issue #58: `est::spawn()` - explicit ownership for fire-and-forget dispatch, and `future<T>` becomes `[[nodiscard]]`

Investigated the issue's own two claims before implementing anything.
Re-reading `future<T>::promise_type`'s `initial_suspend()`/
`final_suspend()` (both `std::suspend_never`) confirmed the issue's own
later "correction": an unobserved coroutine already runs to completion
correctly today, independent of whether anything keeps its returned
`future<T>` alive - that invariant needed no fixing. The real, narrower
gap: an unobserved *failure* was silently dropped, since
`future_state<T>::then()`'s unwrapped-mode auto-propagate path just routes
it into a downstream `future_state` that's also typically discarded.

Also checked #58's stated dependency, issue #103 (a central, id-keyed
waiter registry) - still open, still just a design pass with no
implementation and no decision among its own four candidate shapes
(synthetic-id + linear scan, address-as-id, generational slot map,
in-place tagging). Decided not to block on it: `spawn()`'s actual need is
narrower than what #103 solves (retrofitting eager cancellation into
*existing*, shared waiter lists) - just a *new*, loop-owned collection of
in-flight tasks, each removed by its own completion continuation, with
none of #103's harder cases. Built as its own small mechanism mirroring
`loop::pending_timers_`'s already-proven shape (#103's own "option A"),
scoped to `spawn()` alone - reconcile with #103's registry later if that
ever lands.

**`est::spawn()`** (new, `est/src/spawn.cppm`): `spawn(loop&, future<T>,
Priority prio = current_priority())` and a callable overload that invokes
`fn()` eagerly and forwards to the first. Registers a `then_fast()`
continuation reporting an unhandled exception (via
`loop::spawn_exception_hook()` if installed,
`default_spawn_exception_hook()` otherwise - exempting
`operation_cancelled`/`detail::abandoned_exception` as routine
cancellation/shutdown outcomes, not bugs) and reclaiming its own tracking
entry. `est/src/loop.cppm` gained the generic primitive this needs:
`detail::spawned_entry` (type-erased base, mirroring `ready_node`/
`timer_node` - `:loop` still never names `future<T>`),
`track_spawned()`/`untrack_spawned()`/`spawned_count()`, and a nullable
`exception_hook_type` customization point. `drain_pending()` now also
unconditionally clears `spawned_`, since an abandoned (never `run()`)
completion continuation never reaches `untrack_spawned()` on its own.

**`future<T>` is now `[[nodiscard]]`** - a deliberate follow-up decided
mid-implementation, not part of the original issue text: without it,
nothing stopped a caller from bypassing `spawn()` entirely and just
discarding a coroutine call or a `.then()` chain's tail as before,
undermining the whole point of giving `spawn()` a sanctioned place to
apply an exception policy. `[[nodiscard]]` on the `future<T>` class
itself, not on individual producer functions (`make_ready_future<T>()`,
etc.) - the only mechanism that also catches a user's own coroutine call
being discarded, since a per-function attribute can only ever reach
functions this module declares. `est::detail::discard(future<T>)` (new,
`future.cppm`) is the internal escape hatch for this module's own
genuinely deliberate discards - not exported, so it can't double as a
second, quieter way for external code to bypass `spawn()`. Updated every
site this touched: `with_stop.cppm` (x3), `with_timeout.cppm`,
`when_all.cppm`, `when_any.cppm`, `when_any_succeeds.cppm` (x2),
`spawn.cppm`'s own `then_fast()` call - all wrapped in `discard()`;
`examples/sleep_sort/main.cpp`, `examples/digit_recall/main.cpp` (x2)
routed through `est::spawn()` instead, consistent with
`examples/spreadsheet/main.cpp`'s own `run_server()` (already
`spawn()`-based, and the original motivating case for this issue); a
handful of genuinely deliberate test-only discards (`mutex_tests.cpp`,
`loop_tests.cpp`, `future_tests.cpp` x6) `(void)`-cast, the standard
escape hatch for code outside the `est` module itself.

Caught a real, previously-unnoticed bug the moment this landed:
`mutex_tests.cpp`'s own `holder(loop, m, std::move(release_future));`
was silently discarding a coroutine's future - exactly the pattern this
change exists to catch. Fixed the same way as the other test-only sites.

Two rounds of clang-tidy fixes on the way here:
`NOLINTNEXTLINE(bugprone-empty-catch)` on `default_spawn_exception_hook()`'s
two deliberate empty catches (the `operation_cancelled`/
`abandoned_exception` exemptions); a coroutine test helper switched from
a `bool&` to a `bool*` parameter
(`cppcoreguidelines-avoid-reference-coroutine-parameters`); exception-hook
lambda parameters switched from `std::exception_ptr` to `const
std::exception_ptr&` (`performance-unnecessary-value-param`, also applied
to `loop::exception_hook_type`'s own signature for consistency); and
`last_diagnostic.value_or("").contains(...)` instead of `->find()` after a
`has_value()` `REQUIRE()` (`bugprone-unchecked-optional-access` doesn't
recognize that as a narrowing guard - `spsc_ring_tests.cpp`'s own doc
comment already established this exact pattern).

One more real bug caught by the full pipeline, not just review:
`examples/multicolor_larson_scanner/web/CMakeLists.txt` hardcodes its own
copy of `est`'s source list (a separate CMake project, separate toolchain
- doesn't reuse `est/CMakeLists.txt`'s `FILE_SET`) and was missing the new
`spawn.cppm` partition, breaking that build with "module 'est:spawn' not
found" until added there too.

10 new `TEST_CASE`s in `est/tests/spawn_tests.cpp`: successful completion
+ untracking, the default hook firing/being suppressed for
`operation_cancelled` and an already-abandoned future (produced via
`with_timeout()` + `loop.drain_pending()`, since `detail::abandoned_exception`
isn't nameable from outside the module), a custom hook seeing everything
unfiltered, the callable overload's eager invocation, `Priority` stamping
and default-inheritance, a genuinely-suspending coroutine with no other
observer, and teardown reclaiming a never-completed entry.

Full pipeline: `default` build + 327/327 `ctest` passed, `clang-format`
clean, `ci` build + `clang-tidy` clean (0 warnings), coverage 97% diff
coverage, `sanitize` 270/270 (ASan+UBSan clean), wasm32 build + Node
smoke test clean.

---

## Verification for M0

Once the Dockerfile's toolchain pins are filled in (see "Known open items"):
- `docker build -f docker/Dockerfile .` succeeds locally.
- Inside the container: `cmake --preset default && cmake --build --preset default`
  builds the placeholder `est` module, its Catch2 test, and `hello_world`
  without errors.
- `ctest` passes (the one placeholder test).
- Running `examples/hello_world`'s binary prints its placeholder output.
- `clang-format --dry-run --Werror` is clean over the tree.
- `clang-tidy` is clean over the tree.
- Push a branch; confirm both GitHub Actions workflows go green on the PR.

What's already verified as of this commit, against the locally available
Clang 18.1.3 / CMake 3.28.3 / Ninja 1.11.1 (not the pinned 22/23 toolchain —
see "Known open items"):
- The module-partition + `FILE_SET CXX_MODULES` + Ninja build mechanics:
  `est` (primary module + partition), `est_tests` (Catch2, fetched live via
  `FetchContent` — `github.com` was reachable from this session even though
  `apt.llvm.org`/`cmake.org` were not), and `hello_world` all configure,
  build, and their tests/binary run correctly. This caught and fixed a real
  bug: `enable_testing()` was originally called *after* `add_subdirectory(est)`
  in the top-level `CMakeLists.txt`, which silently made CTest discover zero
  tests.
- `clang-format --dry-run --Werror` and `clang-tidy` (using this repo's own
  `.clang-format`/`.clang-tidy`) both run clean over every `.cpp`/`.cppm`
  file. This also caught and fixed two real issues: `Standard: c++23` isn't
  a value this clang-format version accepts (changed to `Standard: Latest`,
  which works across versions); and the unanchored `HeaderFilterRegex:
  '(est|examples)/.*'` matched the substring "est" inside unrelated paths
  (e.g. a build directory literally named `tidytest`), so it's now anchored
  to `(^|/)(est|examples)/`. `cppcoreguidelines-avoid-do-while` is disabled
  because Catch2's `REQUIRE`/`CHECK` macros expand to a `do-while` at the
  call site, which header filtering can't suppress.

None of this is a substitute for testing against the actual pinned
toolchain once it exists, but it means the scaffolding isn't untested
guesswork either.

---

### Issue #58 revisited (PR #122 review): `set_spawn_exception_hook()` keeps the hook non-empty

A PR review comment on `spawn.cppm` pointed out a gap: `loop`'s hook slot
started genuinely empty until a caller (or `spawn()` itself) touched it,
so `loop::set_spawn_exception_hook(loop_ref, nullptr)` could leave it
empty instead of resetting to the default - the invariant should be that
the hook always points at something, so a reset means "back to the
default," not "back to nothing." `:loop` itself can't provide this
directly - it has no way to name `default_spawn_exception_hook` (the
module DAG forbids `:loop` importing `:spawn`) - so the fix lives at the
layer that can: `est::set_spawn_exception_hook(loop&, hook)` (new, in
`:spawn`) is now the preferred entry point, translating a null/empty
`hook` into installing `default_spawn_exception_hook` explicitly instead
of forwarding the empty value through. `spawn()` itself also self-heals a
still-untouched hook to the default the first time it's read, so its
completion continuation can call `loop.spawn_exception_hook()`
unconditionally with no empty-vs-installed branch. Added a test asserting
the invariant directly at the storage level (`loop.spawn_exception_hook()`
is truthy right after a `nullptr` reset), not just observed indirectly
through `spawn()`'s own behavior. Replied on the review thread and
resolved it.

### Issue #58 revisited: dropped `spawn()`'s tracking collection

Pushback on the implementation's complexity (a suggestion to look at
`intrusive_list` + a new `remove()` method) led to re-examining the
tracking layer added in the original #58 change
(`detail::spawned_entry`/`detail::spawn_entry<T>`,
`loop::track_spawned()`/`untrack_spawned()`/`spawned_count()`). Reading
`intrusive_list.cppm`'s actual source first: it's deliberately
singly-linked (only `next`, matching FIFO enqueue/dequeue in O(1)) and
purely a non-owning linkage structure - removing an arbitrary non-head
node still needs an O(n) predecessor walk, no better than the tracking
collection's existing `find_if` over a `std::pmr::vector`. So the
suggested change wouldn't have simplified anything on its own.

The actual simplification was different: drop the tracking collection
entirely. `spawn()`'s `then_fast()` continuation already keeps
`future_state<T>` alive via its own `owner_` reference (`est:future`) -
exactly like `with_stop()`/`with_timeout()`/`when_all()`/`when_any()`/
`when_any_succeeds()` already rely on, none of which track anything
extra. Correctness never depended on the tracking layer either:
`future<T>::promise_type`'s `suspend_never` initial/final suspend (the
original #58 investigation's own finding) already means a coroutine runs
to completion regardless of whether anything observes its returned
future. The tracking layer's only real, distinct payoff was
`loop::spawned_count()`, an in-flight-task count nothing in this codebase
reads except its own tests - not worth ~60 lines of type-erased base
class, concrete wrapper, and `find_if`-based bookkeeping kept around
speculatively (`loop::pending_timers_`'s own doc comment already makes
the same call for timer tracking, for the same reason: "nothing needs to
walk them... beyond what [the underlying structure] already provides").

Removed: `detail::spawned_entry` (`loop.cppm`), `detail::spawn_entry<T>`
(`spawn.cppm`), `loop::track_spawned()`/`untrack_spawned()`/
`spawned_count()`, the `spawned_` member and its unconditional
`drain_pending()` clear, and `spawn.cppm`'s now-unused
`:util.current_loop` import (`current_allocator_new_delete` was only used
by the removed `spawn_entry<T>`). `spawn()`'s future<T> overload is now
~10 lines: self-heal the hook, then `detail::discard(std::move(task)
.then_fast(...))`. `spawn_tests.cpp` dropped every `spawned_count()`
assertion; the teardown test (spawn a never-completed future, tear the
loop down, confirm no leak) now asserts allocation/deallocation balance
via a `counting_resource`, the same pattern `future_tests.cpp`/
`loop_tests.cpp`/`with_timeout_tests.cpp` already use, instead of an
in-flight count before/after.

Full pipeline re-run clean: `default` build + 328/328 `ctest` passed,
`clang-format` clean, `ci` build + `clang-tidy` clean (0 warnings from
this codebase's own files), 95% diff coverage, `sanitize` 271/271
(ASan+UBSan clean), wasm32 build + Node smoke test clean.

---

### Issue #58 revisited (PR #122 review): `spawn()` resolves `current_loop()`, no explicit `loop&`

Two more review comments on the same PR: `spawn(loop&, ...)`'s explicit
`loop&` parameter should instead be read from "the tls global variable"
(`est::current_loop()`, `:util.current_loop` - the same thread_local
ambient mechanism `make_promise_future()`/`sleep_for()`/`sleep_until()`/
`yield_execution()`/`est::mutex`/`est::counting_event<Mode>` already
resolve through when called without an explicit loop); and the exception
hook's per-call self-heal check (`if (!loop_ref.spawn_exception_hook())`
inside every `spawn()` call, added in the previous review round) "should
be a global initialization" instead.

Checked existing call sites before implementing: `examples/spreadsheet/
main.cpp`'s `run_server()` was already writing `est::spawn(est::current_loop(),
...)` - fetching the ambient loop by hand just to pass it back in - and
every other call site (`spawn_tests.cpp`, `examples/sleep_sort`,
`examples/digit_recall`) already wraps its whole body in
`est::make_current_loop(loop)` before ever calling `spawn()`. Confirmed
the parameter was genuinely redundant everywhere it's used today, not
just in the abstract.

Both comments turned out to describe one coherent change. `spawn()`
(both overloads) dropped its `loop&` parameter entirely and now resolves
`current_loop()` internally. Since that's the only thing spawn() reads a
loop through, "global initialization" of the exception hook became: a
new `est::make_current_loop_with_spawn(loop&)` (`est/src/spawn.cppm`) -
a thin wrapper around `est::make_current_loop()` (`:util.current_loop`)
that also installs the default hook (if nothing has already set a
different one) before returning the same RAII guard. Registration is now
the one place this check runs, not every `spawn()` call. Plain
`est::make_current_loop()` is untouched and still correct for a loop that
never calls `spawn()` at all - every other `current_loop()`-based entry
point ignores this hook regardless.

Considered leaving `spawn()` silently trusting a hook that might still be
empty (a loop registered via plain `make_current_loop()` by mistake,
without the wrapper) rather than adding anything back to spawn()'s own
body - rejected: an empty `std::function` call fails as an opaque
`std::bad_function_call` (or worse, UB under `-fno-exceptions`) exactly
when a genuine bug is what the hook exists to report, the opposite of
this feature's own purpose. Added one `est::check()` precondition inside
`spawn()` instead - checked, not self-healing - naming the fix
(`est::make_current_loop_with_spawn()`) in its failure message, the same
"checked precondition, clear message" idiom `current_loop()`/
`current_allocator()` themselves already use for their own "nothing
registered" case. `:spawn` gained `import :check;` and
`import :util.current_loop;` for this - no new cycle, `:util.current_loop`
already sits below `:spawn` in the DAG (it imports `:loop`, not the
reverse).

Updated every call site: `spawn_tests.cpp` (all `make_current_loop()` →
`make_current_loop_with_spawn()`, all `spawn(loop, ...)` → `spawn(...)`),
plus a new test case asserting `make_current_loop_with_spawn()` preserves
a hook set before registration rather than clobbering it; `examples/
spreadsheet/main.cpp`'s `run_server()` lost its now-redundant
`est::current_loop()` argument entirely; `examples/sleep_sort/main.cpp`,
`examples/digit_recall/main.cpp` (x2) dropped their `loop,` arguments and
switched their one registration call to the `_with_spawn` variant.
`est::check()`'s own failure path (`platform::assert_failure()`,
`[[noreturn]]`, calls `std::abort()`) isn't unit-testable without
process-isolation tooling this project doesn't have (`check_tests.cpp`'s
own doc comment already establishes this) - only the pass-through
(hook-installed) path is covered, matching that file's own precedent.

One clang-tidy fix on the way here: `bool(loop_ref.spawn_exception_hook())`
→ `static_cast<bool>(...)` (`modernize-avoid-c-style-cast`).

Full pipeline re-run clean: `default` build + 329/329 `ctest` passed,
`clang-format` clean, `ci` build + `clang-tidy` clean (0 warnings from
this codebase's own files), 96% diff coverage, `sanitize` 272/272
(ASan+UBSan clean), wasm32 build + Node smoke test clean.

---

### Issue #58 revisited (PR #122 review): spawn()'s exception hook moves to a `thread_local` in `:spawn`, decoupled from `loop`

A third review comment on the same PR, on `loop.cppm`'s now-removed
`spawn_exception_hook_` member: "I think the Hook should be thread local
global hook in the spawn module, for now it doesn't need to be connected
to the loop at all."

Correct, and it dissolves both pieces of machinery the previous two
review rounds had just added. Reporting an unhandled exception from a
spawned task is a per-thread policy (which `platform::printdbg()` to
call, which exceptions are routine) - not a property of any one `loop`
object, and this codebase already has exactly one `thread_local` "current
loop" per thread/core (`:util.current_loop`'s own `tls_context`) to begin
with, so tying the hook to a specific `loop` instance bought nothing.
Moved the hook into `est/src/spawn.cppm` itself as
`detail::spawn_exception_hook_storage`, `thread_local`, statically
initialized directly to `default_spawn_exception_hook` - mirroring
`:util.current_loop`'s own `tls_context` shape (a `namespace est::detail`
block for the storage, sandwiched between two `export namespace est`
blocks for the public accessors, same as that file).

Static initialization to a real value from the moment the thread starts
is strictly stronger than the previous round's "installed once at
registration time" - there's no registration step at all now, and no
"not yet installed" state to guard against, so both `est::
make_current_loop_with_spawn()` (the registration wrapper the *previous*
round added) and `spawn()`'s own `est::check()` precondition (added for
the same reason) are gone. `spawn()` itself no longer touches `current_loop()`
at all either: a `then_fast()` continuation already dispatches through
whatever loop owns `task`'s own `future_state`, established when `task`
was created - `spawn()` never actually needed a loop reference for its
own job, only for the hook it no longer stores there. `loop.cppm` loses
`exception_hook_type`, `set_spawn_exception_hook()`,
`spawn_exception_hook()`, and the `spawn_exception_hook_` member entirely
- it goes back to knowing nothing about spawn() at all, the same "`:loop`
never names `future<T>`" purity this file's own top comment already
established for everything else.

One real test-isolation hazard this surfaced: a `thread_local` hook
persists across `TEST_CASE`s in the same binary, unlike the old
per-`loop` storage where each test's own fresh `est::loop` gave it a
clean slate implicitly. A test that installs a custom hook and never
resets it would otherwise leak into whichever test Catch2 happens to run
next. Added `hook_reset_guard` (`spawn_tests.cpp`, anonymous namespace) -
saves `est::spawn_exception_hook()` on construction, restores it on
destruction - and put one in every `TEST_CASE` that could observe or
change the hook. Verified with `est_tests '[spawn]' --order rand
--rng-seed <n>` across several seeds: passes regardless of run order,
confirming the guard actually does its job rather than happening to pass
under Catch2's default (file/declaration) order.

One clang-tidy fix: `bugprone-throwing-static-initialization` flagged the
`thread_local` initializer (`std::function`'s constructor is
conservatively treated as potentially-throwing in general) -
`NOLINTNEXTLINE`, since the actual target is a plain, captureless
function pointer, always within `std::function`'s guaranteed small-object
buffer, so this specific construction can't allocate and can't throw.

Full pipeline re-run clean: `default` build + 328/328 `ctest` passed (one
fewer than the previous round - the "make_current_loop_with_spawn()
preserves a hook set before registration" test no longer applies to
anything and was removed, not replaced), `clang-format` clean, `ci`
build + `clang-tidy` clean (0 warnings from this codebase's own files),
95% diff coverage, `sanitize` 271/271 (ASan+UBSan clean), wasm32 build +
Node smoke test clean.

---

### Issue #123: `estpico`, a fourth `platform::interface` backend for bare-metal ARM under QEMU

The framework's first genuinely bare-metal backend - no OS, no libc in
the usual sense, a real hardware exception model instead of `SIGABRT`.
Target: QEMU's `mps2-an385` machine (an emulated ARM MPS2 FPGA image,
single Cortex-M3) - not a Raspberry Pi Pico/RP2040 as originally scoped;
RP2040 has no mainline QEMU support (an unmerged RFC and a third-party
fork are the only options), while `mps2-an385` is a real, maintained
QEMU machine with a documented memory map and a UART peripheral. The
module kept its original `estpico` name rather than being renamed for
the new target - a cosmetic mismatch, not a functional one.

**Toolchain**: ARM's own prebuilt "LLVM Embedded Toolchain for Arm"
release supplies the sysroot (picolibc-based libc/libc++/libc++abi/
libunwind for `armv7m-none-eabi`, real exceptions/RTTI enabled via its
`_exn_rtti` variant) - building this from source ourselves is a project
this repo doesn't take on. That release's own bundled Clang 19.1.5 is
*not* installed anywhere, though: it can't compile `import std;`
alongside a textual `#include` of the same standard header in one
translation unit (reproduces with nothing but `import std; #include
<string>;` - a real `requires clause differs in template redeclaration`
bug, not a project misconfiguration), and every one of `est`'s own test
files needs exactly that combination (Catch2's `TEST_CASE` macros need a
textual `#include`; `est::` needs `import est;`, which itself does
`import std;`). This image's own newer, pinned Clang handles the
identical combination correctly against the same sysroot - confirmed
empirically before committing to the design - so `cmake/
toolchain-mps2an385.cmake` cross-compiles with *this* image's Clang
against ARM's prebuilt headers/libraries, the same shape
`toolchain-wasm32.cmake` already uses for a third-party sysroot.
`docker/Dockerfile` downloads the release tarball, extracts only the one
variant actually needed (~20MB out of a ~1.1GB extracted release, mostly
that unused bundled Clang), and discards the rest.

**Exceptions work, given the right linker setup.** Real C++ exceptions
(including typed catch, not just `catch(...)`) function correctly on
this target, contrary to an unverified `-fno-exceptions` assumption
carried through the project's own early spiking. Two real gaps, found
and fixed:
1. The EHABI unwind tables (`.ARM.exidx`/`.ARM.extab`) need explicit
   linker-script placement - missing initially, which is exactly why
   `-fno-exceptions` looked necessary rather than untested.
2. Typed catch specifically (not `catch(...)`, which never reads the
   type-matching table) needs `-Wl,--target2=rel`: `.ARM.extab`'s action
   table encodes each catch clause's `type_info*` via an
   `R_ARM_TARGET2` relocation, whose *meaning* is deliberately
   platform-defined by the ARM EABI. `ld.lld`'s default for this target
   is GOT-indirected, but the EHABI unwinder always decodes this field
   as a direct PREL31 offset - confirmed at the byte level (the decoded
   pointer landed on the `.got` slot's own address, one indirection away
   from the real `type_info`), and fixed by forcing the encoding the
   unwinder actually expects.

Rather than a hand-rolled linker script (Spikes A-D's own approach, and
exactly how the `--target2=rel` gap went unnoticed for as long as it
did - a link-time flag isn't a section, so diffing hand-rolled section
*shapes* against a reference script never would have caught it), `src/
link.ld` builds on picolibc's own `picolibcpp.ld` directly (`INCLUDE
picolibcpp.ld`, the same pattern the toolchain's own sample board files
use) - the board file only sets memory-region symbols, and `Reset_Handler`
uses picolibc's own provided symbol names for `.data`/`.bss`/`.tdata`/
`.tbss` init instead of reinventing them.

**`est::platform::clock` replaces `std::chrono::steady_clock` in
`platform::interface`'s own signature.** A real, previously-undiscovered
blocker, found only once actually compiling `est.cppm` itself against
this toolchain (every earlier spike tested the toolchain/exceptions/
Catch2 in isolation, never `est`'s own source): every variant of this
ARM toolchain - all 40, checked - is built with
`_LIBCPP_HAS_NO_MONOTONIC_CLOCK`, which removes `std::chrono::
steady_clock`'s class declaration entirely, not just its
implementation. Since `platform::interface::now()`/`sleep_until()`/
`detect_loop_stall()` name this type directly, and `est::loop`/
`timer_queue`/`jitter` all alias it as their own `clock`, this blocked
compiling the framework's core at all, not just this one backend.
Fixed with a framework-owned vocabulary clock (`est::platform::clock` -
same representation as `steady_clock`, nanosecond resolution, no working
`now()` of its own since nothing ever calls `clock::now()` directly,
only `platform::interface::now()`) and a mechanical type-tag swap
everywhere `std::chrono::steady_clock::time_point`/`::duration` appeared
in `est`'s own source, both existing backends, and every test file that
implements a fake `platform::interface`. Same representation throughout
means this is a rename, not a semantic rewrite - confirmed by the full
hosted `ctest` suite (328/328) passing unchanged after the swap.

**No OS threads at all** (`_LIBCPP_HAS_NO_THREADS` - `<thread>`/
`std::this_thread` don't exist on this target, unlike `estwasm`'s
genuinely-threaded WebAssembly target). `EST_NO_THREADS` (a
project-owned macro, set in the toolchain file) gates
`larson_scanner_app.cppm`'s one `std::this_thread::yield()` backoff
hint out; `est/tests/spsc_ring_tests.cpp` (spawns a real `std::jthread`
producer to test `spsc_ring<T>` under genuine concurrent access -
meaningless without real concurrency) is excluded from this target's
test binary entirely, not worked around.

**`sleep_until()`'s naive busy-poll was a real performance bug**, found
only once a real application (three `est::schedule_periodic()` timers)
exercised it - `est`'s own test suite never does, since every test fakes
`platform::interface` rather than using the real backend. ARM
semihosting's `SYS_ELAPSED`/`SYS_TICKFREQ` (used for `now()`, since it's
a portable, verified monotonic clock with zero board-specific timer
register knowledge needed - see the issue's own comment for how this was
chosen over a hand-rolled CMSDK timer or DWT cycle-counter driver) are
each a real trap out to the host QEMU process; polling `now()` on every
iteration of `sleep_until()`'s busy-wait turned three timers each waking
every few tens of milliseconds into tens of real seconds per few
simulated seconds. Fixed with a runtime-calibrated spin (one `now()`/
busy-loop round-trip, measured once and cached) that covers the bulk of
a wait without any semihosting calls at all, falling back to the same
`now()`-polling loop only to correct whatever the estimate under/
overshot by - a handful of iterations at most, not thousands.

**mps2-an385 has no working general-purpose GPIO** (QEMU's 4
`cmsdk-ahb-gpio` blocks are present in the device tree but stubbed as
`unimplemented-device` - confirmed via `-d guest_errors,unimp`, writes
are silently discarded) - but does model 8 real LEDs on its System
Control block (`mps2-scc`, register `CFG1` at `0x4002f004`, bits
`[7:0]`, active-high, verified via write-then-readback with zero
guest-errors). The ported `examples/multicolor_larson_scanner/
mps2an385` drives these directly (`app_config{.width = 8}` - not a
downscaled approximation of some larger strip, the animation is
configured for the hardware it actually has), thresholding each pixel's
brightest channel into an on/off bit since the SCC's LEDs have no PWM/
analog output. No button/switch input is modeled at all (the SCC's own
DIP-switch register is a read-only stub, always zero), so unlike the
terminal/wasm ports there's no input thread - bare-metal single-core has
no real OS thread to run one on regardless - the app instead runs for a
fixed frame budget then exits via semihosting, a deterministic shape
suited to a QEMU-driven CI smoke test rather than a demo someone drives
by hand.

One more bug worth naming, found only by tracing all the way through:
after fixing `sleep_until()`'s performance and confirming (via temporary
UART tracing) that `drain_commands()` correctly processed the quit
command and called `loop_.stop()`, the program *still* never exited.
Cause: `main()`'s own `return 0;` at the end - a bare-metal `main()`
returning doesn't terminate anything, it falls into `Reset_Handler`'s
own trailing `for(;;){}` (which exists precisely because there's no OS
to return *to*). `std::exit(0)` (routing through picolibc's normal
`_exit()` chain, which this board's `startup.c` wires to the same
semihosting `SYS_EXIT_EXTENDED` call every other termination path uses)
was the fix - the same thing `src/test_main.cpp` already did correctly
from having been modeled on the spike work's own proven pattern.

**Real Catch2 v3.7.1, with real exceptions enabled** (not
`CATCH_CONFIG_DISABLE_EXCEPTIONS` - that mode's failing-`REQUIRE`-aborts-
the-whole-binary limitation no longer applies once exceptions genuinely
work), patched at build time for the handful of things this target can't
provide - no filesystem (`--output-file`/`-f`/Bazel sharding all fail
loudly if ever reached, which this project's own fixed `argv` never
does), no monotonic clock for Catch2's own internal timing (cosmetic
only - doesn't affect `REQUIRE`/pass-fail reporting), no
`std::random_device` (falls back to `std::time(nullptr)` seeding, the
same bar `jitter`'s own `get_random_seed()` documents). The user's own
preference over a hand-maintained fork: a checked-in unified-diff patch
(`third_party/catch2-baremetal.patch`) applied to a `FetchContent`-fetched
checkout via `PATCH_COMMAND`, verified to apply cleanly against a fresh
upstream `v3.7.1` clone. `tests/CMakeLists.txt` deliberately doesn't run
Catch2's own top-level `CMakeLists.txt` (which assumes a hosted build
throughout) - it reuses just `CMake/CatchConfigOptions.cmake` (Catch2's
own option-default script) before building the (patched) library itself
from the fetched source directly.

Result: `est`'s own full test suite runs for real under QEMU - 1110
assertions, 259 test cases, all passing (`spsc_ring_tests.cpp` excluded,
per above) - not just a toolchain smoke test. `run_under_qemu.sh` turns
a built ELF's real QEMU exit code (via `SYS_EXIT_EXTENDED`) into a
checkable result; deliberately not wired through `ctest`/`add_test()` -
this project builds inside the pinned devenv image, which has no reason
to also carry `qemu-system-arm`, the same "build in docker, verify with
a runner-native tool" split `ci.yml`'s own `wasm` job already
established for its Node-based smoke test.

**Issue #123 follow-up: `estpico`'s clock stops depending on the host to
function; `platform::interface::now()` becomes `uptime()`.** The
`estpico` backend above still had its monotonic clock (and, downstream
of it, `get_random_seed()`) sourced entirely from ARM semihosting's
`SYS_ELAPSED`/`SYS_TICKFREQ` - a real trap out to whatever's actually
running the CPU on every single call. Under QEMU (with `-semihosting`,
as every invocation in this project already passes) that's QEMU's own
process servicing the trap in software, not a real debug host - but the
underlying mechanism is the same one a real board would need an actual
attached debug probe for, meaning this "bare-metal" backend's basic
sense of time only worked with a host attached. Raised directly by the
user: a real firmware shouldn't need a host just to know how long it's
been running.

Ruled out one alternative before building the other: keeping the
semihosting *instruction* convention but adding our own fault handler to
answer it directly (so the same `bkpt 0xab` call site works whether or
not a host is listening) was rejected - that's solving a privilege-
boundary problem (why `SVC`/similar traps exist at all: so unprivileged
code can ask privileged code to do something on its behalf), and this
Cortex-M3 target has no privilege separation to cross in the first
place. Rigging a trap-and-decode path to reach the exact register read a
plain function call already reaches is indirection with no payoff here.

Built directly on real hardware instead: the Cortex-M3 core's own
SysTick timer (`0xE000E010`, core-internal - no board-specific address
lookup needed, unlike the CMSDK peripherals). `CLKSOURCE=1` (the
processor clock)'s actual frequency under this exact QEMU
version/machine isn't documented anywhere this project trusts blindly -
measured empirically instead, the same methodology `SYS_TICKFREQ`'s own
1e9 figure was confirmed with earlier: a temporary firmware enabled
SysTick with max reload and `CLKSOURCE=1`, busy-polled `COUNTFLAG` across
30 full wraps, and bracketed the whole loop with `SYS_ELAPSED` reads as
an already-trusted independent ground truth (the temporary firmware was
deleted once the number was in hand; the technique is in this module's
own doc comment for the next person who needs to re-verify it against a
different QEMU version). Result: **24,998,930 Hz**, i.e. a clean 25 MHz -
matching the commonly documented MPS2 AN385 default HCLK, and letting
`ticks_to_time_point()` become exact 64-bit integer multiplication (40ns/
tick) instead of the old `long double` frequency conversion. The same
30-wrap loop's own real wall-clock duration (~20.1s for 20.13s of
computed SysTick time) also confirmed SysTick is tied to QEMU's real/
virtual clock the same way the semihosting counter was, not raw
instruction-count throughput - timing behavior carries over unchanged.

SysTick's 24-bit counter wraps every ~671ms at 25MHz, so a real
`SysTick_Handler` (wired into `startup.c`'s vector table at slot 15,
defined in `platform_mps2an385.cppm` with `extern "C"` linkage so the C
startup file's forward declaration resolves it at link time) extends it
into the 64-bit tick count `platform::clock` promises, incrementing a
`volatile std::uint64_t` wrap counter once per wrap. Reading that counter
back alongside the hardware's own current-value register needed real
thought: naively reading both separately races the ISR (interrupt
latency means there's a real, if tiny, window after the hardware
auto-reloads but before `SysTick_Handler` has actually run, during which
a naive read would undercount by a full wrap). This is the first place
`estpico` has ever needed genuine interrupt-context protection -
different from, and not covered by, the "two coroutines interleaving at
a `co_await`" hazard `est::mutex` exists for (`docs/wiki/
Architecture.md`'s "single-threaded, no atomics" section now says so
explicitly) - so `systick_ticks()` wraps the paired read in a new
`estpico::detail::interrupt_guard` (a short `PRIMASK` mask/restore RAII
guard), cheap enough (a handful of cycles) to pay on every call, unlike
the semihosting trap it replaced.

That cost drop had a second, unplanned payoff: the old `now()` was
expensive enough (a real host round-trip) that a plain `while (now() <
deadline) {}` `sleep_until()` cost tens of real seconds per simulated
second, which is why the previous entry's `calibrated_ns_per_spin_
iteration()`/`busy_spin()` machinery existed at all. `systick_ticks()`
is cheap enough that `sleep_until()` goes back to a plain polling loop -
deleting that whole calibration apparatus rather than adapting it.
Measured effect: `est`'s own QEMU test suite (1110 assertions, 259
cases) dropped from ~1.6s real to ~0.67s real; `larson_scanner_mps2an385`
(200 rendered frames) stayed at ~6.7s real for ~6.6s simulated, matching
its pre-change behavior exactly, confirming the swap changed *how* time
is measured without changing what it measures.

`platform::interface::now()` is renamed to `uptime()` across every
backend (`estext`/`estwasm`/`estpico`) and every call site
(`loop.cppm`/`timer.cppm`/`timer_periodic.cppm`/`promise.cppm`/
`with_timeout.cppm`/`with_stop.cppm`, every test fake) - a
purely mechanical rename, but a meaningful one: the contract was always
"monotonic time since this backend's own arbitrary epoch" (process/board
start, the same contract `std::chrono::steady_clock::now()` itself
carries), never wall-clock "the current time," and `now()` invited the
wrong reading.

The one place semihosting is still used at all: `terminate()`'s
`SYS_EXIT_EXTENDED` call, which `assert_failure()` also reaches. Kept
deliberately - it's not part of "functioning," it's "tell whatever's
running me the process outcome," which only matters for the QEMU/CI use
case this backend is explicitly scoped to (real flashed firmware never
calls it: nothing here returns from `main()` or asserts under normal
operation). One accepted, pre-existing gap worth naming plainly: on real
hardware with no debug host attached, a failed `est::check()` would now
fault at that `bkpt` instead of exiting cleanly - unchanged by this
work, and out of scope for a backend that already documents itself as
QEMU-only.

**Issue #123 follow-up 2: `estpico`'s UART output becomes asynchronous
and interrupt-driven.** Raised alongside the clock fix above, same
session: `vprintdbg()`'s old implementation (`detail::uart_write()`,
straight busy-wait polling one character at a time) blocks its caller
for however long the whole message takes to physically leave the UART -
fine for the rare, fatal `assert_failure()` message, wrong for a debug
diagnostic that might fire from inside a hot continuation. Asked for
specifically: a fixed-capacity queue an interrupt drains, fed by a
buffer manager tying queued messages together in a list, pumped by an
`est::spawn()`'d task at `Priority::high` (not `critical`).

Before writing any of it, two real hardware facts needed confirming
empirically, the same methodology the clock fix used - neither is
documented anywhere this project trusts blindly:

- **Which NVIC line UART0's TX-complete interrupt uses.** Found by
  wiring every external IRQ slot (0-31) in a temporary vector table to
  one shared handler that reports its own exception number (read via
  `mrs %0, ipsr`, external IRQs start at 16) back through a global,
  enabling every candidate line via `NVIC_ISER0 = 0xFFFFFFFF`, then
  writing one byte to `UART0_DATA` and observing which line actually
  fired. Answer: **IRQ1** (confirmed twice - once with every UART CTRL
  interrupt-enable bit set, once with only `TX_EN`/`TX_INT_EN`, ruling
  out RX/overrun as the real source).
- **How the interrupt actually behaves.** Two things weren't obvious
  from the register layout alone: whether enabling `TX_INT_EN` on an
  already-idle UART fires it immediately (it does not - confirmed by
  enabling it with zero prior writes to `UART0_DATA` and seeing no
  interrupt until software wrote the first byte itself, meaning the
  interrupt is edge-latched on "a transmission just completed," not a
  sustained level on "buffer currently empty"), and whether it needs
  explicit acknowledgement (it does - `UART0_INTSTATUS` at offset `0xC`,
  write 1 to bit 0 to clear; *not* acking it produced a real storm, 21+
  re-fires from a single byte before a safety valve kicked in; acking it
  produced exactly one fire per byte). Confirmed together with a
  temporary test string streamed entirely by the ISR itself, one byte
  per interrupt, no further mainline writes after the first.

That same temporary test also demonstrated a real hazard the design
already needed to account for: the test's own mainline diagnostic print
(polled, uncoordinated) and the ISR both writing `UART0_DATA`
concurrently produced visibly garbled, interleaved output - direct,
reproduced evidence for the "ISR can preempt mainline at any point"
hazard `estpico::detail::interrupt_guard` (the clock fix's own addition,
above) exists to close, now needed for a second piece of shared state.

**Design landed:**
- `estpico::detail::tx_buffer : est::intrusive_list_node` - one queued,
  formatted message; `pending_buffers` (an `est::intrusive_list<tx_buffer>`)
  and `draining_buffer` (the one currently being fed into the ring,
  tracking its own partial-drain offset) are both mainline-only - the TX
  ISR never touches either, only the ring below.
- `est::spsc_ring<char>` (64 bytes, a function-local `static` rather than
  a plain global so its constructor's real `pmr` allocation defers until
  well after `main()` starts, not during static init before the heap is
  necessarily live) is the one genuine ISR/mainline boundary -
  `est::spsc_ring<T>`'s own doc comment already named "a future
  bare-metal interrupt handler" as a design target for this exact shape.
- `pump_uart_hardware()` (ack INTSTATUS, pop one byte if TX isn't full)
  is called from *both* the real ISR and, wrapped in `interrupt_guard`,
  from mainline to prime a cold start (the interrupt only ever fires on
  a completed transmission, never spontaneously - the empirical finding
  above) - one function, never running concurrently with itself either
  way (the ISR can't be preempted by mainline; the guard blocks the
  reverse), keeping `uart_tx_ring` down to one logical consumer despite
  two call sites.
- `pump_some()` (drain `pending_buffers`/`draining_buffer` into the ring
  until either caught up or the ring fills) is the one piece of logic
  both `enqueue_output()`'s synchronous fast path and `pump_task()`'s
  spawned, yielding loop share - most messages are short enough to
  fully drain in the first call, needing no coroutine at all.
- `pump_task()` is only `est::spawn()`'d (at `Priority::high`, exactly
  as asked) when `pump_some()` reports genuine backlog *and*
  `est::has_current_loop()` - a small, new, non-asserting query added to
  `est:util.current_loop` (`current_loop()`/`current_allocator()` both
  assert if nothing is registered, which `vprintdbg()` can't risk: it's
  called with no loop current at all by
  `est/tests/platform_tests.cpp`'s own existing "vprintdbg() writes...
  without throwing" test, run unchanged against this exact backend under
  QEMU - confirmed still passing, its message short enough to never need
  the coroutine path).
- `assert_failure()` deliberately does *not* go through this queue - it
  calls `drain_uart_tx_synchronously()` first (flushing anything
  `vprintdbg()` already had in flight, so earlier debug context isn't
  silently lost right before the process halts), then writes its own
  message through the same synchronous `uart_write()` as before,
  unconditionally, since there's no later point after `terminate()` at
  which a merely-queued message would ever reach the wire.
- `uart_putc()`/`uart_write()` (still assert_failure()'s and the drain
  function's own byte-level primitive) now wrap in `interrupt_guard`
  too - without that, any code still using the plain synchronous writer
  (this backend's own `test_main.cpp`, whose `uart_streambuf` redirects
  *all* of Catch2's own console output through the identical `UART0_DATA`/
  `STATE` registers) would race the new TX ISR exactly the way the
  temporary discovery firmware demonstrated.

Verified end to end: `est`'s own QEMU test suite (1110 assertions, 259
cases, including the vprintdbg-with-no-loop test above) and
`larson_scanner_mps2an385` (200 rendered frames) both still pass, with
completely clean, non-garbled UART output despite Catch2's own console
reporter and the new interrupt-driven diagnostic path sharing the same
wire throughout the run. Full hosted pipeline (clang-format, `ci`
build+tidy, tests, new-code coverage gate, `sanitize` build+test) also
green - `has_current_loop()` picked up its own direct unit test
(`est/tests/loop_tests.cpp`) once diff-cover flagged it as the one
genuinely uncovered line the `est` core change added.

**PR #124 review, round 2: the pump task becomes a persistent,
event-woken loop.** Two review comments on `pump_task_running` (the
manual bool tracking "is a pump episode currently in flight"): first,
replace it with a held `future<void>` clone (`future<T>::clone()`'s own
doc comment already names this exact use) - `nullopt`/`ready()` both
mean "not running," nothing to reset on completion. Landed as a2fddd5.
Then a second, larger suggestion: skip the per-episode spawn entirely -
one persistent `pump_task_loop()`, spawned once, `co_await`ing an
`est::binary_event<EventResetMode::automatic>` in an infinite loop,
rather than a fresh coroutine per backlog episode.

Landed the second version. `est::binary_event<Mode>` (a `counting_event<Mode>`
saturated at `max_count = 1`) turned out to fit cleanly: its constructor
needs no loop at all (safe as a plain module-level global, like
`systick_wrap_count` above, just not a POD this time), and `set()`
itself only resolves `current_loop()` once a waiter is actually queued
(its own doc comment) - meaning `enqueue_output()` can ring the event
unconditionally once a loop is current, with no separate "has the task
ever run" gate needed on that side either, beyond the one
`pump_task_started` bool guarding the single, one-time `est::spawn()`
call itself (a real bootstrap flag, not the same "per-episode" state the
first review comment removed). `automatic` mode (not `manual`): the
single unit saturates harmlessly across however many `enqueue_output()`
calls land while the task is already busy, since `pump_some()` drains
everything currently queued in one pass regardless of how many separate
`set()` calls contributed to the backlog - `manual` mode's "every future
wait() also takes the fast path until reset()" isn't needed here.

One behavioral simplification came with it: `enqueue_output()` no longer
tries a synchronous fast path once a loop is current at all - every call
just enqueues and rings the event, fully deferring to
`pump_task_loop()`, since `pump_some()` already handles "fits in one
shot" (the common case) exactly as well from inside the coroutine. The
no-loop-current fallback (`vprintdbg()` called before any loop exists,
or `est/tests/platform_tests.cpp`'s own test exercising this directly)
still calls `pump_some()` synchronously though - nothing would ever wake
a task that can't be spawned in the first place, and losing that path
would mean a diagnostic emitted before `main()`'s own loop starts is
silently queued forever instead of reaching the wire.

Verified again end to end: mps2an385 QEMU test suite (1113 assertions,
260 cases, including the vprintdbg-with-no-loop test) and
`larson_scanner_mps2an385` both still pass, same real-time behavior as
before either round of this review.

**PR #124 review, round 3: `sleep_until()` gets a real `wfi` + a second
timer, not just a busy-poll.** A third review comment, this time on
`sleep_until()`'s own plain `while (uptime() < deadline) {}` loop: use
`wfi` (Wait For Interrupt), "a standard arm32 function," to actually
halt the CPU between checks instead of spinning it.

The real complication, surfaced before writing any code: `wfi` wakes on
*any* enabled interrupt, and the only periodic one wired up (SysTick)
only fires once per ~671ms wrap - but `larson_scanner`'s own
`tick_interval`/`render_interval`/`command_poll_interval` are all
20-33ms. A naive `wfi` with nothing else providing a closer wakeup would
have meant waiting up to ~671ms past every real deadline - not a
rounding error, a ~20x frame-rate collapse. Fixing it for real needed a
*second*, independent timer that could be armed per-deadline: SysTick's
own reload is load-bearing for `systick_ticks()`'s own tick accounting
(its formula assumes every wrap represents exactly one full
`systick_cycle_length`), so temporarily shortening it to align with an
arbitrary sleep deadline would desynchronize the clock itself - not
something worth risking for a wakeup mechanism.

The board's second CMSDK APB timer peripheral (the "dual-timer," at
`0x40002000` - confirmed via `info mtree`, same as every other
peripheral base in this project) was sitting there unused. Its own
register layout (a standard ARM SP804-shaped block: `LOAD`/`VALUE`/
`CONTROL`/`INTCLR` per sub-timer, `CONTROL` bits for `ONESHOT`/`SIZE`/
`INTEN`/`ENABLE`) and its NVIC wiring for this specific board
(`IRQ10`) came from QEMU 8.2.2's own device model source
(`hw/timer/cmsdk-apb-dualtimer.c`, `hw/arm/mps2.c`) - fetched as a
starting hypothesis, then run through the identical two-step discipline
every other piece of hardware in this file got: confirmed empirically
against the real binary, not trusted from the source alone. `IRQ10`
checked out via the same shared-handler/IPSR technique used for UART0
TX; acking via `T1INTCLR` turned out to be required the same way UART0's
own interrupt was - a real 200+-fire storm resulted from *not* acking
it, exactly one fire from acking it. `TIMCLK` (the timer's own input
clock) was calibrated directly against SysTick's own already-trusted
`uptime()` rather than semihosting this time - both derive from the
same board clock, and it came out to the identical clean 25 MHz, so the
same 40ns/tick integer math applies.

`sleep_until()`'s new shape: recompute the remaining time each loop
iteration, arm the dual-timer for a one-shot interrupt at (approximately)
that remaining duration, `wfi`, recheck `uptime()` against the real
deadline (since `wfi` could have woken on an unrelated interrupt - a
UART TX completion, say), and repeat if not actually there yet. A
deadline already in the past never touches the timer at all.

Verified two ways: `est/tests/platform_tests.cpp`'s own "sleep_until()
returns once the deadline has passed" test already exercises the real
(not faked) `platform_mps2an385::sleep_until()` with a genuine 1ms
deadline, unmodified, still passing. More tellingly,
`larson_scanner_mps2an385`'s 200-frame run stayed at the identical
~6.7s real time as every prior measurement in this file - had the
naive-`wfi` oversleep problem actually landed, 200 frames at up to
~671ms each would run for minutes, not seconds. One concrete, measured
benefit past correctness: the same run's host-side CPU time (`user` in
`time`'s own output) dropped from ~6.6s to ~0.18s - `wfi` is genuinely
idling the emulated CPU between events under QEMU's own TCG, not just a
correctness no-op.

**PR #124 review, round 4: one shared source list instead of three
hand-copied ones.** A fourth review comment, this time on the build
system rather than estpico itself: `est/CMakeLists.txt`,
`examples/multicolor_larson_scanner/web/CMakeLists.txt`, and
`.../mps2an385/CMakeLists.txt` each declare their own `add_library(est
STATIC)` (none of the three standalone cross-compile projects can
`add_subdirectory()` the real `est/CMakeLists.txt` - see those two
files' own top comments) - but all three had been hand-copying the
exact same 23-entry `FILE_SET CXX_MODULES` file list verbatim, a real
maintenance hazard every time a `.cppm` file gets added to `est/src/`.

Fixed with `cmake/EstSources.cmake`: a single `EST_CXX_MODULE_SOURCES`
list, paths bare (relative to `est/src`, no project-specific prefix),
`include()`-d by all three consumers, each then `list(TRANSFORM ...
PREPEND ...)`-ing its own correct absolute base path before handing the
result to `target_sources()` - the one thing that genuinely differs
between the root build and the two standalone projects. Everything else
(warnings/coverage/sanitizers for the root build; the deliberate absence
of them for the two cross-compiles) stays exactly as it already was in
each file.

Verified by a fresh `rm -rf build/*` + reconfigure + full build (not an
incremental one, to actually exercise `include()`/`list(TRANSFORM)`
running from scratch) in all three: hosted `default` preset (329/329
tests), `web`'s wasm32 project (smoke test still passing), and
`mps2an385` (1113 assertions/260 cases under QEMU, `larson_scanner`
still exit 0) - all unchanged from before the refactor, confirming this
touched only where the file list lives, not what gets built.

**Post-review follow-up: two real bugs in `pump_task_loop()`, found by a
requested code review, then fixed by removing the coroutine entirely.**
Before merging, a code review pass (focused on the new estpico platform
code specifically) surfaced two issues in the UART TX pump design that
round 2's own review (above) had settled on:

1. `est::spawn(pump_task_loop(), est::Priority::high)`'s `Priority::high`
   never actually applied to the coroutine's own ongoing work.
   `promise_type::initial_suspend()` returns `std::suspend_never`
   (`est/src/future.cppm`), so calling `pump_task_loop()` runs its body
   synchronously, on the caller's own stack, up to its first `co_await` -
   *before* `est::spawn()` is even entered as a function call. `spawn()`'s
   own `prio` argument only ever gets stamped onto the one `then_fast()`
   completion continuation it registers on the already-built future
   (`est/src/spawn.cppm`) - for a `while (true)` coroutine that never
   completes, that continuation is structurally unreachable. The
   coroutine's real resumption priority came from whatever
   `current_priority()` happened to be at its call site inside
   `enqueue_output()` instead - `Priority::normal`, the thread-local
   default (`est/src/loop.cppm`), in every real call path - and then
   self-propagated at that level forever (issue #31's inheritance
   behavior), never touching `Priority::high` at all.
2. The inner `while (!pump_some()) { co_await est::yield_execution(); }`
   retry (only reachable when a single `enqueue_output()` call's text
   exceeds the 64-byte ring in one pass) was a genuine poll, not a wait:
   nothing in the design ever produced a "ring gained space" event -
   `yield_execution()` just re-enqueues the coroutine and it rechecks
   next turn, unconditionally, however many times it takes the real TX
   interrupt to drain enough of the ring.

The fix isn't a better-behaved retry: it's removing the coroutine's role
in draining the ring at all. Making the *ISR* signal a real
`est::binary_event`/wake the coroutine directly was considered and
rejected - `est::loop`'s ready-queue and `est::counting_event`'s waiters
list are plain, non-atomic `intrusive_list`s, mutated by ordinary
mainline code with interrupts enabled; an ISR calling into either from
inside `UART0_TX_Handler` could preempt mainline mid-mutation of the
exact same list, a real data race this codebase's own "single-threaded,
no atomics" stance (CLAUDE.md) doesn't cover and shouldn't be made to,
for one platform-specific backend.

Instead, `pump_some()` (renamed `refill_ring()`) becomes a pure "top the
ring off from `pending_buffers`/`draining_buffer`, stop the moment it's
full" function with no hardware/kick logic of its own, and
`pump_uart_hardware()` (already the real `UART0_TX_Handler`, already
called from mainline via `uart_tx_kick()`) calls it after every byte it
pops - so the ring gets refilled by the same interrupt that just freed a
slot in it, every time, with no coroutine involved in the loop at all.
`enqueue_output()` drops to appending a buffer and kicking the hardware
once; `est::spawn()`, `pump_wake_event`, `pump_task_started`, and
`pump_task_loop()` are gone entirely - there's no persistent coroutine
left for a priority argument to fail to reach, and no retry loop left to
poll. The one new correctness obligation this creates -
`pending_buffers`/`draining_buffer` are now touched by the real ISR too,
not just mainline - is handled the same way every other ISR/mainline
hazard in this file already is: `interrupt_guard` around
`enqueue_output()`'s own `pending_buffers.enqueue()` call.
`est::spsc_ring<T>`'s own lock-free index handoff needed no change - its
"never called concurrently with itself" contract for `try_push()`/
`try_pop()` holds regardless of which physical context calls either, and
`interrupt_guard`/ISR-non-preemption already guarantee that.

Verified end to end: the full mps2an385 QEMU test suite (1114 assertions,
261 cases - a new case added specifically for this, a 96-byte message
forcing several real ISR-driven ring refills, confirmed to print intact
and complete near-instantly, not stalling on any retry) and
`larson_scanner_mps2an385` (exit 0, same real-time behavior, confirmed
unrelated to this change via a baseline comparison against the
pre-fix code) - plus the full hosted pipeline (`default`/`ci`/`sanitize`
presets, `clang-tidy`, the diff-coverage gate) and the wasm32 project,
none of which this backend-specific change touches but all of which
still pass since `est/tests/platform_tests.cpp` (the one shared file
that gained the new long-message test case) is common to all of them.

**`est::spawn()`'s Fn&& overload now actually changes the created task's
priority, not just its completion report.** A follow-up to the previous
entry's `Priority::high` finding: that bug was specific to
`pump_task_loop()` (since removed), but the mechanism behind it - a
`future<T>` overload's `prio` reaching only the one completion
continuation `spawn()` itself registers, never the task's own synchronous
prefix or later resumptions - is a real, general property of `spawn()`,
not an estpico-only quirk. Raised directly: `spawn()` should take a
callable rather than a `future<T>` for the version of the call meant to
actually change a task's priority, since only `spawn()` itself - invoking
the callable, not just receiving its already-run result - is ever in a
position to raise `current_priority()` before that first synchronous
prefix runs.

Fixed by having the `Fn&&` overload wrap its call to `fn()` in
`set_priority(prio)` (`est/src/spawn.cppm`): `fn()`'s return - typically
a coroutine call - now runs its synchronous prefix under the raised
priority, so its first suspension point gets `prio` stamped onto it
directly (rather than whatever was ambient at the real call site), and
every later `co_await` inherits it in turn (issue #31's own
priority-propagation behavior). The `future<T>` overload is unchanged -
structurally it can't do this, since by the time a caller has a
`future<T>` to hand it, that prefix has already run - but its own doc
comment now says so explicitly, alongside the `Fn&&` overload's, so the
next reader doesn't have to rediscover the gap the hard way `estpico` did.
`docs/wiki/Coroutines.md`'s own `est::spawn()` section updated to match -
the callable overload was previously documented as pure "eager-call
sugar," which is no longer the whole story.

New test (`est/tests/spawn_tests.cpp`): a coroutine that records
`current_priority()` both in its own synchronous prefix and again after a
real `co_await est::yield_execution()` resumption, spawned via the
`Fn&&` overload at `Priority::high` - both observations come back
`Priority::high`, proving the raised priority survives an actual
suspend/resume, not just the synchronous call. Verified end to end: the
full hosted pipeline (`default`/`ci`/`sanitize` presets, `clang-tidy`,
the diff-coverage gate - `spawn.cppm`/`spawn_tests.cpp` both at 100%
diff coverage), the mps2an385 QEMU test suite (1116 assertions, 262
cases) and firmware smoke test, and the wasm32 project - `spawn()` is
core `est`, shared by every backend, so every target that links it needed
re-verifying, not just the one estpico had originally surfaced the gap
in.

**Reworking the UART TX pump again: `refill_ring()` was doing a heap
deallocation from ISR context.** Flagged directly: `pump_uart_hardware()`
(the real `UART0_TX_Handler`) calling `refill_ring()`, which calls
`draining_buffer.reset()` once a buffer is fully drained - a real
deallocation (frees the `tx_buffer`'s own `std::string data`, then the
node itself) through whatever general-purpose heap allocator
`std::unique_ptr` uses here, from inside an ISR. Nothing documents that
allocator as safe to reenter while mainline might already be
mid-allocation/mid-free on the same heap - a genuine, silent
heap-corruption hazard distinct from (and worse than) the non-atomic
`intrusive_list` mutation hazard `interrupt_guard` already existed to
rule out; masking interrupts around the *call* does nothing about the
call itself being unsafe to make from an ISR in the first place. Missed
in the previous round's own review of this exact design.

Reworked to keep `pending_buffers`/`draining_buffer` strictly
mainline-only again (matching round 2's original shape) - the real ISR
(`pump_uart_hardware()`) goes back to only ever popping one byte and
writing it to `UART0_DATA`, no refilling, no allocation, nothing beyond
raw MMIO and `uart_tx_ring`'s own lock-free `try_pop()`. All the actual
byte-shoveling work (`pump_some()`, restored to its round-2 shape) moves
back to `pump_task_loop()`, a persistent coroutine `est::spawn()`'d once
- now via the `Fn&&` overload specifically (the previous entry's own
fix), so `Priority::high` actually reaches its ongoing work this time,
not just a never-reached completion continuation.

The part that actually changed from round 2: what happens when the ring
fills before a buffer is fully queued. Round 2 retried via
`co_await est::yield_execution()` - a real busy-poll, no actual "ring has
space" signal backing it (this session's earlier finding). Signaling that
from the ISR directly was considered and rejected - `est::loop`'s
ready-queue and `est::counting_event`'s waiter list are exactly the kind
of non-atomic structure an ISR can't safely touch either, the same class
of hazard as the deallocation above, just at the coroutine layer instead
of the allocator layer (issue #125 sketches what a real ISR-safe wake
primitive would need). Replaced with a real, non-busy backoff instead:
`co_await est::sleep_for(1ms * queued_buffer_count)`, `queued_buffer_count`
being a plain mainline-only counter (`tx_buffer` isn't kept in anything
with an O(1) `size()`) tracking how many buffers are currently
backlogged. Not a precise wait - a deliberate heuristic, scaling the
sleep with how much is queued rather than retrying at a fixed interval
regardless of backlog size.

Verified: the mps2an385 QEMU test suite (1116 assertions, 262 cases,
including the existing 96-byte long-message case, still passing) and the
`larson_scanner_mps2an385` firmware smoke test, plus the full hosted
pipeline (`platform_tests.cpp`'s own long-message test is shared across
backends).

**A second, more serious bug found in the course of that verification,
not yet fixed**: `pump_task_loop()` never terminates and is spawned
exactly once, against whichever `est::loop` is current the first time
it's needed - but nothing ever tells it to stop before that loop goes
away. Confirmed with a throwaway instrumented build of
`larson_scanner_mps2an385` (a `printdbg()` call injected into the first
rendered frame, so a real message is enqueued against the program's one
real loop): the message drains correctly in full, but the program then
crashes on exit with `est::current_loop(): no loop is current`. Sequence:
the coroutine finishes draining and goes back to
`co_await pump_wake_event.wait()`, parked in `pump_wake_event`'s own
`waiters_` list - not `est::loop`'s `ready_`/`pending_timers_`, so
nothing drains it when the owning loop is destroyed normally at the end
of `app::loop()`. `pump_wake_event` itself is a process-lifetime global,
so it only gets destroyed later, at `std::exit()`'s static-destruction
phase - by which point no loop is registered at all, and completing the
still-parked coroutine's own `future_state<void>` (to abandon it) needs
`current_loop()` to hand off its resume node. This is exactly the
documented `future_state<T>` precondition
("callers are responsible for not interleaving distinct est::loop
registrations across the lifetime of a single future_state/promise/future
chain" - its own doc comment, `est/src/future.cppm`), violated by a
process-lifetime coroutine parked on a process-lifetime event outliving
the one loop it was ever actually spawned against. The exact same
`platform_tests.cpp` long-message test hit a narrower version of this
too (a per-`TEST_CASE` loop, constructed and destroyed within one test,
corrupting a *later* test's own printdbg() call) - fixed there by not
constructing a loop in that shared test at all (this file's own updated
comment on that test has the reasoning), which avoids triggering it but
doesn't fix the underlying gap.

Not fixed in this round - needs a real answer for "who tells the pump
task to stop, and when," which this codebase doesn't have a hook for yet
(`platform::interface` has no "the loop is about to go away" callback,
and `platform_mps2an385`'s own destructor never runs in the actual
`std::exit()`-based shutdown path `main.cpp`/`test_main.cpp` both use).
Local commit only, not pushed - flagged for discussion before deciding
which direction to take it.

**Two corrections to the backoff/stop design, and a first (incomplete)
attempt at the shutdown gap.** Two direct corrections to the previous
entry's design:

1. The backoff sleep should scale with `uart_tx_ring`'s own fixed
   capacity, not the current backlog - `1ms * uart_tx_ring_capacity`
   (~64ms: roughly how long the *whole* ring takes to drain via real
   UART transmission, not tied to how much is actually queued), not
   `1ms * queued_buffer_count`. Simpler too: `queued_buffer_count` (a
   counter added purely to size the old formula) is gone entirely, along
   with its increment/decrement in `enqueue_output()`/`pump_some()`.
2. `pump_task_loop()` now takes an `est::stop_token` and races both of
   its awaits against it (`est::with_stop(pump_wake_event.wait(), token)`,
   `est::sleep_for(pump_backoff_delay, token)`), inside one `try`/`catch`
   for `est::operation_cancelled` around the whole loop - a real, tested
   way for the coroutine to actually exit instead of staying parked
   forever, directly addressing the previous entry's crash. A new
   `pump_stop_source()` (lazily-constructed, same `est::spsc_ring`-style
   reason as `uart_tx_ring()`) backs it, with `estpico::
   request_uart_tx_pump_stop()` exported as the public way to fire it.

The "who calls it, and when" half is still open, and turned out
harder than it looked. `est::future_state<T>::complete()` (reached from
`est::promise<T>::set_value()`, which `request_stop()` calls) resolves
`current_loop()` *unconditionally*, before it even checks whether
anything is registered to hand off - so `request_stop()` itself asserts
if called with no loop current, not just the coroutine it's meant to
wake. `platform_mps2an385`'s own destructor - the first place this looked
for a hook - turned out to be dead code for this purpose: both
`main.cpp` and `test_main.cpp` call `std::exit()` rather than returning
normally from `main()`, which skips local destructors entirely,
confirmed by adding (then removing) a diagnostic `printdbg()` at the top
of the destructor that never printed in either binary's own output.

Tried calling `request_uart_tx_pump_stop()` from *inside* `main.cpp`'s
own render callback instead - the one place a real loop is genuinely
current - a few frames before requesting the loop itself stop (leaving
room for the resulting wake/cancel cascade to actually run before
`est::loop::stop()`, which only returns after finishing whichever single
ready continuation is *currently* running, cuts it short). This didn't
reliably fix the crash: it passed or failed depending on unrelated code
changes nearby (an unrelated diagnostic `printdbg()` call flipped a
consistently-reproducing failure to a consistently-reproducing pass, and
neither a 5-frame nor a 20-frame margin changed that on its own) -
meaning the actual mechanism still isn't understood, not that more
margin would fix it. Pulled the whole attempt back out of `main.cpp`
rather than ship something whose correctness depends on unrelated code
nearby. `request_uart_tx_pump_stop()` itself stays exported as a real,
correct building block - just not proven safe to call from any specific
place in this codebase yet.

Verified: mps2an385 QEMU test suite (1116 assertions, 262 cases) and
`larson_scanner_mps2an385`'s own normal (non-instrumented) 200-frame run
both still pass - this round's changes are additive/corrective to the
pump task's own internal behavior, not a regression in anything already
working. The shutdown crash itself remains open, same as the previous
entry. Local commit only, not pushed - per the same instruction as
before.

**Code review pass before pushing: one real bug, one comment-placement
fix.** Requested once the two local commits above were ready to go out.

1. `enqueue_output()` set `pump_task_started = true` *before* calling
   `est::spawn(...)`, not after. `spawn()`'s own argument evaluation can
   throw (coroutine frame allocation, `pump_stop_source()`'s own lazy
   construction, `pump_wake_event.wait()`'s waiter-node allocation) -
   since `vprintdbg()` wraps the whole call in `catch (...)`, such a
   throw is silently swallowed, but `pump_task_started` stayed latched
   `true` regardless, so every *later* `enqueue_output()` call (loop
   still current) would only ever append to `pending_buffers` and ring
   an event nobody's waiting on - async debug output would silently and
   permanently stop after one transient allocation failure, contradicting
   this file's own stated "best-effort" contract. Fixed by moving the
   flag write to *after* `spawn()` returns, so a throw leaves it `false`
   and the next call retries.
2. Two doc comments (`pump_stop_source()`'s and
   `request_uart_tx_pump_stop()`'s) had drifted into narrating the
   specific debugging session that produced them - "an attempt... was
   tried and pulled back out," "confirmed... with an instrumented
   build" - duplicating, in the source itself, exactly the story this
   file's own PLAN.md entries already tell. Trimmed to state only what's
   true of the code *now* (the precondition, why a destructor-based hook
   doesn't work, that nothing calls the stop function yet), per CLAUDE.md's
   own "comments reflect current state, not narrated history" rule -
   the removed narrative isn't lost, it's already recorded above,
   unchanged.

Verified again: mps2an385 QEMU test suite (1116 assertions, 262 cases)
and `larson_scanner_mps2an385`'s normal run both still pass; `clang-format`
clean. Pushed.

**Issue #128: `est/CMakeLists.txt` made self-contained, so the standalone
cross-compile projects can `add_subdirectory()` it instead of
hand-declaring their own `est` target.** Before this, `est/CMakeLists.txt`
called `est_set_warnings()`/`est_enable_coverage()`/`est_enable_sanitizers()`
unconditionally (functions only defined once the root `CMakeLists.txt` had
already `include()`-d `cmake/CompilerWarnings.cmake` and friends) and read
`EST_BUILD_TESTS` as a variable it never declared itself - both fine for
the root build, which always sets those up first, but exactly why
`examples/multicolor_larson_scanner/web` and `.../mps2an385` (each a fully
separate CMake project - see their own top comments) couldn't
`add_subdirectory()` the real file and instead hand-declared their own
`add_library(est STATIC)` from `cmake/EstSources.cmake`'s shared file list
(PR #124 round 4's own fix for the previous hazard, three hand-copied
source lists - see that entry above).

Fixed by making `est/CMakeLists.txt` self-contained: it now declares its
own `option(EST_BUILD_TESTS ...)` and calls `enable_testing()` itself
(harmless to call twice - the root's own call, needed for `examples/*/
tests`, is unaffected), and the three warnings/coverage/sanitizers calls
are each wrapped in `if(COMMAND est_set_warnings)` etc. - real, applied
calls for the hosted build (root already `include()`-d those modules
before `add_subdirectory(est)`), no-ops otherwise. `web/CMakeLists.txt`
and `mps2an385/CMakeLists.txt` now `set(EST_BUILD_TESTS OFF)` (their own
`tests/` reuses `est/tests/`'s source files directly instead -
`est/tests/CMakeLists.txt`'s own `FetchContent`-based Catch2 assumes the
hosted toolchain) then `add_subdirectory("${EST_ROOT}/est" ...)`, replacing
their own hand-rolled `include(EstSources.cmake)` + `add_library(est
STATIC)` block. `cmake/EstSources.cmake` itself is unchanged in shape,
just now included solely from `est/CMakeLists.txt`.

Verified inside the devenv container: a fresh `default` preset
configure+build+`ctest` (331/331 tests) confirms the hosted build is
unaffected - in particular that the guarded `est_set_warnings()` etc.
calls still actually fire there (checked `compile_commands.json` for
`est/src/loop.cppm`: still compiled with the full `-Wall -Wextra ...
-Werror` set). A fresh `web`'s wasm32 project configure+build+smoke test
passes with the new `add_subdirectory(est)` line in place of the old
hand-rolled block. `mps2an385`'s own configure reaches CMake's Generate
step (past all of `est/CMakeLists.txt` and its own `add_subdirectory(est)`
call) before failing on a missing `libc++.modules.json` under
`/opt/arm-none-eabi-sysroot/lib/` - reproduced identically against the
unmodified `origin/main` version of `mps2an385/CMakeLists.txt` on the same
image, confirming it's a stale/incomplete local devenv image (missing the
ARM sysroot's own std-modules metadata), not something this change caused.

**Follow-up (PR #129 review):** the reviewer asked whether
`cmake/EstSources.cmake` was still needed, now that it's `include()`-d
from exactly one place. It wasn't - the separate-file split existed
solely to share one list across three hand-rolled `add_library(est
STATIC)` declarations, a need issue #128 above already removed. Inlined
`EST_CXX_MODULE_SOURCES` directly into `est/CMakeLists.txt` and deleted
`cmake/EstSources.cmake`; the two standalone projects are unaffected,
since they only ever consumed the list indirectly through
`add_subdirectory(est)`, never by `include()`-ing the file themselves.
Verified: `default` preset build+`ctest` (331/331) and `web`'s wasm32
project build both still pass unchanged.

**Follow-up: `spsc_ring_tests.cpp` on `mps2an385`.** The whole file was
excluded from `est_mps2an385_tests` (PR #124's own list, above) solely
because its last `TEST_CASE` spawns a real `std::jthread` producer -
`<thread>` doesn't exist on this `EST_NO_THREADS` target. Every other
case in that file is pure single-call-stack logic (FIFO order,
wraparound, full/empty boundaries, move-only `T`, reject-on-full) that
needs no threading at all, so excluding the entire file threw away real
coverage of `spsc_ring<T>`'s own logic for no reason tied to those cases
themselves.

Split the one `std::jthread` test out into a new
`est/tests/spsc_ring_thread_tests.cpp`, leaving `spsc_ring_tests.cpp`
genuinely single-threaded throughout (its own header comment already
claimed this; the jthread test at the end had made it false). Added the
new file to `est/tests/CMakeLists.txt` (hosted build, unaffected -
both files already ran there). Added `spsc_ring_tests.cpp` to
`mps2an385/tests/CMakeLists.txt`'s `est_mps2an385_tests` sources;
`spsc_ring_thread_tests.cpp` stays excluded there, with an updated
comment explaining why (now pointing at the specific file, not the
whole ring).

Verified inside the devenv container: hosted `default` preset
build+`ctest` (all 12 `spsc_ring`-tagged cases pass, split correctly
across both files) and `clang-format`/`clang-tidy` clean on both new/
changed files. `mps2an385`'s own cross-compile configure+build now
succeeds cleanly through to a working `est_mps2an385_tests` binary
(earlier session notes on this branch recorded a missing
`libc++.modules.json` blocking Generate - not reproduced this time, the
devenv image apparently gained the ARM sysroot's std-modules metadata
since). Run for real under `run_under_qemu.sh`: 1256 assertions, 273
test cases, all passing - up from PR #124's original 1110/259, the
delta being `spsc_ring_tests.cpp`'s own cases now actually running on
real target hardware instead of being silently absent.

**Follow-up: a real ISR-driven `spsc_ring` test on `mps2an385`.** The
split above only recovered the single-call-stack logic coverage;
`spsc_ring<T>`'s actual reason to exist - the atomic acquire/release
protocol between two genuinely different execution contexts - still had
no coverage on this target at all (`spsc_ring_thread_tests.cpp` needs
real OS threads, which don't exist here). Added one: a real interrupt
racing the mainline producer, not another single-threaded simulation.

Rejected reusing `UART0_TX_Handler` directly - it's tied to real
hardware timing/state (edge-latched on "a transmission just completed,"
only fires after the first byte primes it), nothing this test needs.
Rejected a second free-running hardware timer too - the only spare one
(the SP804 dual-timer) is the exact peripheral `sleep_until()` already
arms per call; running it periodic for this test's duration would
contend with any other test in the same binary that happens to sleep.
Landed on: self-triggering an otherwise-permanently-idle external
interrupt line (IRQ6, "GPIO0" - startup.c's own vector table, never
asserted by anything real on this board) directly via the NVIC's own
Interrupt Set-Pending Register (`0xE000E200`, architectural SCS address,
same on any Cortex-M3). This is a real ISR entry - genuine register
save/restore, genuine interrupt arbitration - fully under test control,
with no hardware contention.

`startup.c` (shared with the real firmware, `larson_scanner_mps2an385`):
repointed IRQ6's vector table slot from `Default_Handler` to a new
`GPIO0_Handler`, declared `__attribute__((weak, alias("Default_Handler")))`
so every build without a strong override - i.e. every build except the
test binary - keeps today's behavior exactly. New
`examples/multicolor_larson_scanner/mps2an385/tests/spsc_ring_isr_tests.cpp`
(this project's own `tests/`, not `est/tests/` - it needs real NVIC
registers, not backend-agnostic) defines the strong `GPIO0_Handler`:
one `try_pop()` per firing, matching `UART0_TX_Handler`'s own "one unit
of work per interrupt" shape. Mainline spins `try_push()` (matching
`estpico::enqueue_output()`'s own retry-on-full shape) across 500 items
against an 8-slot ring, firing the interrupt both on every full-retry
and opportunistically every third iteration - the latter is what
actually lands preemption mid-`try_push()`, not just between calls.

The one thing worth getting right: the ISR-visible drained-items buffer
is a fixed `std::array<int, N>` + `std::atomic<int>` index, not a
`std::vector`. A vector's `push_back()` can reallocate, and heap
(de)allocation from interrupt context is the exact hazard this
codebase already hit and fixed for real in estpico's own UART TX pump
(this file's own "ISR deallocation hazard" entry, above) - a
fixed-capacity array sidesteps the question rather than leaning on a
`reserve()` call never being exceeded. `std::atomic` (not a plain
`int`) on the index is load-bearing too, for a different reason: the
compiler has no visibility into an MMIO write meaning "an unrelated
global might now change," so a plain int in the mainline polling loop
could get hoisted out entirely.

Verified inside the devenv container: `clang-format` clean on both
changed/new files; `mps2an385` cross-compile (both the test binary and
`larson_scanner_mps2an385` itself, confirming the shared `startup.c`
change is safe) build cleanly; run for real under `run_under_qemu.sh` -
274 test cases/1258 assertions (up from 273/1256), and
`larson_scanner_mps2an385` itself still exits 0 under QEMU, confirming
IRQ6's weak default behaves exactly as before everywhere except the one
binary that overrides it.

# `est` — an educational async framework for C++23/26

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

### M3 — the looper
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

### M4 — coroutine adapters
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

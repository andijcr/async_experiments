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
    requires re-validating this in the same commit. **Not yet pinned** —
    see "Known open items" below.
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
├── CMakePresets.json              # "default" (local/dev) and "ci" configure presets
├── cmake/
│   ├── CompilerWarnings.cmake    # shared warning flags for est targets
│   └── toolchain-hosted-linux.cmake  # compiler/stdlib pin, used by the presets
├── docker/
│   └── Dockerfile                # the one image for local dev + CI
├── est/                          # the platform-agnostic framework library
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── est.cppm              # primary module interface (re-exports partitions)
│   │   ├── placeholder.cppm      # est:placeholder — M0 walking-skeleton partition
│   │   ├── platform/platform.cppm  # est:platform — hosted-Linux HAL backend
│   │   ├── sync/mutex.cppm       # est:sync.mutex
│   │   ├── timer.cppm            # est:timer
│   │   ├── future.cppm           # est:future — shared_state<T>, future<T>
│   │   └── promise.cppm          # est:promise — promise<T>, make_promise_future()
│   └── tests/
│       ├── CMakeLists.txt
│       ├── skeleton_tests.cpp
│       ├── platform_tests.cpp
│       ├── mutex_tests.cpp
│       ├── timer_tests.cpp
│       └── future_tests.cpp
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
`coroutine.cppm` in M4). This tree is not a frozen contract.

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

---

## `.clang-format` / `.clang-tidy`

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
  backend is a new template argument, not a redesign.
- `est::timer_queue<Platform, Allocator>`, in `est/src/timer.cppm`
  (`est:timer`) — a `std::vector`-backed binary min-heap of one-shot
  deadlines (`schedule_at`/`schedule_after`, `cancel` — O(n) linear-scan
  cancel, deliberately simple for M1 — `next_deadline`, `pop_ready`),
  templated on the allocator per docs/PLAN.md's allocator-first design.
  No callbacks/continuations yet — that's `est::loop`'s job in M3; this
  is only the scheduling structure a future loop will own and drive.
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
- `shared_state<T>`, in `est/src/future.cppm` (`est:future`) — the single
  owned object behind both handles. Holds value-or-exception storage
  (`std::variant<std::monostate, T, std::exception_ptr>`), a continuation
  list built on `est::waiter_list` (extracted from `est::mutex` in a
  later review round — see below), and a plain (non-atomic —
  single-threaded) reference count. Always heap-allocated via
  `std::pmr::polymorphic_allocator<std::byte>` — see
  `make_promise_future()` below — never constructed directly by a
  caller. Not templated on a generic `Allocator` the way
  `est::timer_queue` is: `shared_state`/`future`/`promise` cross an API
  boundary (many call sites, needs a uniform, non-template-parameterized
  type), which is exactly the case docs/PLAN.md's "Allocator support"
  section already called out for `std::pmr::polymorphic_allocator`-style
  erasure — `est::timer_queue` stays a generic `Allocator` template
  parameter because it doesn't cross such a boundary (one component, one
  concrete instantiation). `get()` uses C++23 deducing-this: called on an
  lvalue it returns `const T&` (non-consuming, safe for multiple readers);
  called on an rvalue (`std::move(state).get()`) it returns `T&&`, an
  explicit opt-in to move, same caveat as `std::optional<T>::value() &&`.
- Continuations are `est::detail::continuation_node<T>` (`est:future`,
  non-exported — an implementation detail of `then()`, not part of
  `shared_state`'s public surface), a small virtual `invoke(shared_state&)`
  deriving from the *type-agnostic* `est::detail::waiter_node`
  (`destroy(allocator)` + virtual destructor only — nothing `T`-dependent,
  so it's compiled once instead of once per `T`), itself deriving from
  `est::mutex_waiter` — exactly what `mutex_waiter`'s "payload-free at
  this layer" doc comment in M1 anticipated: no second allocation for the
  list node itself. `future<T>::then(fn)` allocates a concrete
  `continuation_node` wrapping `fn` (via the `shared_state`'s own
  allocator, using C++20's `polymorphic_allocator::new_object`/
  `delete_object`) and hands it to the `shared_state`; completion
  (`set_value`/`set_exception`) drains *every* queued continuation (LIFO,
  same order `est::waiter_list` is documented to use) rather than
  assuming at most one — nothing stops a caller registering more than
  one `then()`, and the underlying list already supports it. `run()`'s
  own destroy-on-exit guard is `est::scope_exit` (`est:util.scope_exit`),
  a small reusable RAII "run this on scope exit" utility, not an ad-hoc
  local struct.
- `est::promise<T>`, in `est/src/promise.cppm` (`est:promise`) — producer
  handle: move-only, `set_value(const T&)`/`set_value(T&&)`/
  `set_exception`.
- `est::future<T>`, in `est/src/future.cppm` — consumer handle: move-only,
  `.then(fn)` where `fn` is called as `fn(shared_state<T>&)` so it can
  `get()` (rethrowing any stored exception) or `ready()`. Runs
  synchronously, on whichever call stack completes the `shared_state` —
  there's no loop yet to defer onto (M3 will change that). `get()` is
  also deducing-this: `future.get()` copy-constructs, `std::move(future)
  .get()` moves — safe unconditionally here since a `future` is a
  single-consumer handle, unlike `shared_state<T>::get()` itself.
- `make_promise_future<T>(allocator)`, in `est/src/promise.cppm` —
  constructs a fresh `shared_state<T>` and returns the `{promise, future}`
  pair sharing it; the only way a `shared_state` is created.
- **View semantics**: both handles are thin (state pointer + move-only
  ownership via `add_ref()`/`release()`); destroying a `future` does not
  destroy the `shared_state` if something else — a still-live
  `est::promise`, and eventually (M3) the loop's own keep-alive
  registration — still references it. Verified directly: dropping a
  `future` while its `promise` is still alive leaves the `shared_state`
  intact and the `promise` can still complete it.
- **Abandoned-future semantics — deferred to M3, not a scope cut.** The
  full version (a dropped `future`'s `shared_state` is kept alive by the
  *loop's* own reference, not the user's, so the async work keeps
  running in the background) needs `est::loop` to exist to do the
  registering — M2 has no loop yet. What M2 *does* build is the
  ref-counted view mechanics that M3 will hook into: `shared_state` isn't
  destroyed while any reference (promise, future, or later the loop's)
  still holds it. The exact "unobserved exception" policy (result/
  exception simply discarded vs. a debug-mode assert or logged warning)
  is still a detail to settle when M3's loop actually implements the
  registration, not now.

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

**Round 5: `import std;`, project-wide.** Closes the long-standing TODO
(M0, "Docker strategy" / "Known open items") that this was ever an
unverified gap in the first place. Two changes make it work:
`cmake/toolchain-hosted-linux.cmake` sets
`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` to `"0e5b6991-d74f-4b3d-a41c-cf096e0b2508"`
(the gate value for CMake 3.30.0-3.31.7 - confirmed via WebSearch this
round, since the original scaffolding session couldn't reach
cmake.org/discourse to check; the pinned `CMAKE_VERSION` (3.31.0,
`docker/Dockerfile`) falls inside that range) before `project()`, where a
toolchain file's content runs; the top-level `CMakeLists.txt` then sets
`CMAKE_CXX_MODULE_STD ON` to actually request the module once the gate
allows it. Every `.cppm`/`.cpp` file's `module; #include <...>;` /
`#include <...>` block became `import std;` (or, for two non-module
`.cpp` files, `import std;` alongside the unaffected `#include
<catch2/...>` - Catch2 isn't a module).

Two headers stayed as plain `#include`s specifically because their
public API is macro-based, and macros are never transmitted across an
`import` (modules carry declarations, not preprocessor state) -
`import std;` genuinely cannot replace them: `<cassert>` in
`future.cppm` (the `assert()` macro), and `<cstdlib>` in
`examples/hello_world/main.cpp` (`EXIT_FAILURE`/`EXIT_SUCCESS`). Both
kept in the smallest scope that still compiles (a `module;` global
module fragment for `future.cppm`, since it's a module interface unit;
a plain top-level `#include` for `main.cpp`, since it isn't one).

**Accepted, deliberate tradeoff: local build verification is gone for
the rest of this session (and this repository, in this sandbox), not
just degraded.** `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`/`CMAKE_CXX_MODULE_STD`
don't exist before CMake 3.30; this sandbox's CMake is 3.28.3, and
upgrading it isn't possible here either (apt.llvm.org is blocked by the
same network egress policy that blocked Docker Hub above - confirmed by
trying it directly, same "policy denial, don't route around it"
outcome). A structural configure+build attempt without the toolchain
file fails cleanly and immediately (`fatal error: module 'std' not
found`) rather than silently, at least - not a subtle miscompile risk,
just a hard stop on the one verification path this session has relied
on for every prior round. Discussed directly with the user before
proceeding (not a unilateral call): confirmed as the intended tradeoff
rather than something to design around. From here on (this PR's
remaining pushes, and M3/M4/M5 later, in this sandbox specifically),
build-level verification is real-CI-only; the established push-then-
watch-CI loop still applies, just without the local pre-check that
usually catches mistakes before spending a CI round-trip on them.

### M3 — the looper
- `est::loop`: single-threaded run loop owning the ready-queue and the
  timer min-heap from M1. `run()` drains ready continuations, sleeps until
  the next timer deadline, repeats; `run_until_idle()` for tests/examples
  that shouldn't block forever.
- Owns/creates the `shared_state`s it's handed (see M2's abandoned-future
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
  registered on the `shared_state`, using symmetric transfer where the
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
  before exiting) rather than the M0 placeholder.
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

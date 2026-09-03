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

### Candidate: gate CI on new-code coverage (not yet decided)

Currently no coverage measurement exists at all — no instrumentation, no
report, nothing. Raised mid-M2: gate CI on *patch/diff* coverage (are the
lines a PR adds actually exercised by a test?), not overall repository
percentage. Leading option: Clang's built-in source-based coverage
(`-fprofile-instr-generate -fcoverage-mapping` + `llvm-cov`, already in
the devenv image via LLVM's `all` install) to produce an lcov report,
intersected with the PR diff via a tool like `diff-cover` to gate on just
the changed lines — keeps everything inside the same self-contained
toolchain this project has otherwise stuck to, at the cost of more setup
than piping to a hosted service like Codecov (which does the diff
intersection and PR annotations for you, but adds an external dependency
this project hasn't otherwise taken on). Not implemented yet — pending a
decision on which way to go.

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
  list built directly on the M1 `est::mutex`'s waiter list, and a plain
  (non-atomic — single-threaded, see `est::mutex`'s own docs) reference
  count. Always heap-allocated via `std::pmr::polymorphic_allocator<std::
  byte>` — see `make_promise_future()` below — never constructed
  directly by a caller. Not templated on a generic `Allocator` the way
  `est::timer_queue` is: `shared_state`/`future`/`promise` cross an API
  boundary (many call sites, needs a uniform, non-template-parameterized
  type), which is exactly the case docs/PLAN.md's "Allocator support"
  section already called out for `std::pmr::polymorphic_allocator`-style
  erasure — `est::timer_queue` stays a generic `Allocator` template
  parameter because it doesn't cross such a boundary (one component, one
  concrete instantiation).
- Continuations are `shared_state<T>::continuation_node`, a small virtual
  base (`invoke(shared_state&)`) deriving from `est::mutex_waiter` —
  exactly what `mutex_waiter`'s "payload-free at this layer" doc comment
  in M1 anticipated: no second allocation for the list node itself.
  `future<T>::then(fn)` allocates a concrete `continuation_node` wrapping
  `fn` (via the `shared_state`'s own allocator, using C++20's
  `polymorphic_allocator::new_object`/`delete_object`) and hands it to
  the `shared_state`; completion (`set_value`/`set_exception`) drains
  *every* queued continuation (LIFO, same order `est::mutex`'s waiter
  list is documented to use) rather than assuming at most one — nothing
  stops a caller registering more than one `then()`, and the underlying
  list already supports it.
- `est::promise<T>`, in `est/src/promise.cppm` (`est:promise`) — producer
  handle: move-only, `set_value`/`set_exception`.
- `est::future<T>`, in `est/src/future.cppm` — consumer handle: move-only,
  `.then(fn)` where `fn` is called as `fn(shared_state<T>&)` so it can
  `get()` (rethrowing any stored exception) or `ready()`. Runs
  synchronously, on whichever call stack completes the `shared_state` —
  there's no loop yet to defer onto (M3 will change that).
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

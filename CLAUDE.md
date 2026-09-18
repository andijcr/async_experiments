# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`est` is an educational, single-threaded async framework for C++23/26, built
from scratch as real C++20 modules (no headers for the framework's own
code). Namespace `est`: a monotonic-clock/assert-failure platform seam, a
run loop, a future/promise pair (also usable as a coroutine return type), an
intrusive-waiter-list mutex, and a small set of allocator-aware utilities
(`shared_ptr`, `intrusive_list`, `scope_exit`). `docs/wiki/Home.md` is the
canonical reader's guide to how the code works today; read it (and the pages
it links) before making non-trivial changes.

## Toolchain

This project needs a pinned **Clang 22/23 snapshot + libc++ + CMake 4.2+ +
Ninja** — `import std;` and C++20 module support at this level don't exist
on typical distro toolchains. Always build/test/lint inside the devenv
container (`docker/Dockerfile`, image `ghcr.io/andijcr/async-experiments-devenv`);
don't assume a bare `cmake`/`clang++` on the host will work.

```sh
docker run --rm -it -v "$(pwd)":/workspace -w /workspace \
  ghcr.io/andijcr/async-experiments-devenv:latest bash
```

(or build it locally: `docker build -f docker/Dockerfile -t est-devenv .`)

All commands below are run from inside that container, at the repo root.

## Common commands

```sh
# Configure + build (Ninja is required; CMakePresets.json wires it up)
cmake --preset default
cmake --build --preset default

# Run all tests
ctest --preset default --output-on-failure

# Run a single test / tag directly through the Catch2 binary
# (est's own tests)
./build/default/est/tests/est_tests "future then chains a continuation"
./build/default/est/tests/est_tests "[mutex]"
# (examples/spreadsheet's own tests)
./build/default/examples/spreadsheet/tests/spreadsheet_tests "[protocol]"

# Format: check, then fix
find est estwasm examples -type f \( -name '*.cpp' -o -name '*.cppm' -o -name '*.h' \) \
  -print0 | xargs -0 clang-format --dry-run --Werror
find est estwasm examples -type f \( -name '*.cpp' -o -name '*.cppm' -o -name '*.h' \) \
  -print0 | xargs -0 clang-format -i

# Lint (needs a completed build first: tidy analyzes a TU that imports a
# module, which needs that module's compiled BMI)
cmake --preset ci && cmake --build --preset ci
find est examples -type f \( -name '*.cpp' -o -name '*.cppm' \) \
  -print0 | xargs -0 -n1 clang-tidy -p build/ci

# Sanitizers (ASan + UBSan; separate build dir, examples excluded)
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize --output-on-failure

# New-code coverage gate (mirrors ci.yml; ci preset already builds with
# -fprofile-instr-generate)
cmake --preset ci && cmake --build --preset ci && ctest --preset ci
llvm-profdata merge -sparse build/ci/profraw/*.profraw -o build/ci/coverage.profdata
llvm-cov export --format=lcov --instr-profile=build/ci/coverage.profdata \
  build/ci/est/tests/est_tests --object=build/ci/est/tests/est_tests > build/ci/coverage.lcov
diff-cover build/ci/coverage.lcov --compare-branch=origin/main --fail-under=80

# examples/multicolor_larson_scanner/web: a fully separate CMake project
# (its own preset, its own toolchain file, cmake/toolchain-wasm32.cmake)
# targeting wasm32-wasip1-threads for a browser Worker/main-thread build -
# not part of any preset above. Node 18+ needed for the smoke test only.
cd examples/multicolor_larson_scanner/web
cmake --preset wasm32 && cmake --build --preset wasm32
node tests/smoke_test.mjs build/wasm32/larson_scanner_wasm
```

CI (`.github/workflows/ci.yml`) runs, in order, the same steps above:
`clang-format` check → configure/build (`ci` preset) → `clang-tidy` → tests
→ coverage gate → configure/build/test again with the `sanitize` preset.
Every step must pass; all of them are required status checks.

## Architecture

### Two modules, one direction

- **`est`** (`est/src/est.cppm`) is the framework itself: one C++ module
  made of partitions (one per source file under `est/src/`), forming a
  strict DAG — `:platform` → `:check`/`:timer` → `:loop` → `:future` →
  `:promise`, with `:sync.mutex` and `:util.current_loop` depending on
  `:loop`/`:future`/`:promise` as needed. `import est;` alone gives zero
  trace of any concrete backend.
- **`estext`** (`estext/src/hosted_stdcpp.cppm`) is a wholly separate
  module holding `estext::hosted_stdcpp`, the one concrete
  `platform::interface` implementation (`std::chrono` clock,
  `std::this_thread` sleep, `std::cerr` diagnostics). It `import est;`s the
  finished product and is never the reverse — a future bare-metal backend
  would be its own similarly separate module.
- A program that wants a working backend does both: `import est; import
  estext;`, constructs a `hosted_stdcpp`, and installs it via
  `est::platform::override_instance()` at the top of its own `main()`
  (see `examples/*/main.cpp`, `est/tests/test_main.cpp`). Nothing installs
  a default backend as a side effect of `import est;`.

See `docs/wiki/Architecture.md` for the full partition dependency graph and
the reasoning behind it (in particular, why `:loop` sits *below* `:future`/
`:promise` in the DAG even though a loop is what runs futures' continuations
— `est::detail::ready_node`/`timer_node` are the type-erased base classes
that make that possible).

### Core pieces

- `est::platform::interface` / `instance()` / `override_instance()` — the
  only runtime-polymorphic seam in the framework (a virtual base swapped via
  a global pointer), covering the monotonic clock and assert-failure
  handling. Everything else is templates/concrete classes.
- `est::loop` — owns a ready-queue and a `timer_queue`; the only thing that
  ever actually invokes a continuation or fires a timer. Threaded explicitly
  (`est::loop&`) rather than a hidden global; `est::current_loop()` /
  `est::make_current_loop()` (`:util.current_loop`) are an opt-in
  convenience layered on top, not a requirement.
- `est::future<T>` / `est::promise<T>` — a `shared_ptr`-backed
  consumer/producer pair over a module-private `future_state<T>`, created
  only via `make_promise_future<T>(loop&)`. `.then()` chains defer through
  `est::loop` instead of running inline; `future<T>` is also the coroutine
  return type (no separate `task<T>`). See `docs/wiki/Continuation-Node-Mechanism.md`
  and `docs/wiki/Allocation-Patterns.md` for the node hierarchy and the
  exact allocation cost of chains.
- `est::mutex` — an intrusive waiter list + lock word (not OS-backed);
  `lock()` is awaitable, guarding a critical section across a coroutine
  suspension point, and `acquire()` returns a `future<lock_guard>` for
  non-coroutine callers.
- `est::shared_ptr<T>` / `est::intrusive_list<T>` (`est/src/util/`) — the
  generic, non-atomic reference-counted pointer and intrusive list every
  owned/queued object above is built on.

### Allocator-first

Every owned object (a `shared_ptr<T>` control block, a continuation node,
`timer_queue`'s storage, `loop`'s own containers) is built through a
`std::pmr::polymorphic_allocator<std::byte>` threaded in explicitly, not
global `new`/`delete`. Keep new owning types consistent with this rather
than defaulting to unconstrained allocation.

Prefer automatic memory management over a naked `new`/`delete` pair: reach
for `est::shared_ptr<T>::make()` (shared ownership) or
`std::unique_ptr<T>`/`std::make_unique<T>()` (single ownership) at the
allocation site. Where a lower-level primitive genuinely needs a raw owning
pointer handed off to something else (`est::loop`'s own `ready_`/
`pending_timers_` containers, which store a plain `detail::ready_node*`/
`timer_node*`), construct the node via `std::make_unique<T>()`, perform the
handoff, and call `.release()` only once that handoff has actually
succeeded - never a bare `new` followed by an unguarded call that could
throw and leak it.

### Single-threaded, no atomics

`est::loop` is driven from exactly one call stack; `shared_ptr`'s ref count
is a plain `int`. This is a deliberate scope boundary, not a gap — don't add
locking/atomics speculatively. `est::mutex` exists for a different,
still-single-threaded hazard: two coroutines interleaving at a `co_await`
while both hold a reference to shared state.

## Docs and comments

- `docs/PLAN.md` is the project's decision log: every milestone, review
  finding, and bug fix, in the order it happened. It's meant to read that
  way — don't "clean up" its historical narration.
- `docs/wiki/` is the reader's guide to how the code works *right now* and
  must stay in sync with the code as it changes.
- Everywhere else — code comments and any other docs — keep comments short
  and to the point. Docs and comments should reflect the current state of
  the codebase, not narrate the historical transformations that produced
  it (that belongs in `docs/PLAN.md`, or in the git history, not in a
  comment or doc page).

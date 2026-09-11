# What `current_loop()`/`.allocator()` actually cost, under LTO

This page traces the real machine code `est::current_loop()` and the
allocator lookup built on top of it produce, in an optimized, whole-program
build — not an estimate, actual disassembly. `examples/probe` holds the
`noinline` call sites this page's listings come from; reproduce with:

```sh
cmake --preset lto
cmake --build --preset lto
objdump -d -C --no-show-raw-insn build/lto/examples/probe/probe
```

(`cmake --preset lto`: `CMAKE_BUILD_TYPE=Release` + `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`,
Clang's `-O3 -flto=thin`.)

## `est::current_loop()`

```asm
mov    current_instance(%rip),%rdi   # global load: platform object ptr
mov    (%rdi),%rax                   # load: object's vtable ptr
jmp    *0x40(%rax)                   # load + indirect tail-jump: vtable slot -> get_current_loop_context()
```

Three instructions, three loads, one indirect branch, zero `call`/`ret` —
`platform::instance().get_current_loop_context()` compiles to a tail call:
the caller needs no stack frame of its own, it just jumps into
`get_current_loop_context()` with the return address already pointing past
the caller. Two of the three loads are genuine pointer chases (global →
platform object, object → vtable); the third is the vtable-slot read the
`jmp` itself performs.

That lands in `hosted_stdcpp::get_current_loop_context()` (`estext`), whose
warm path (no explicit loop registered via `make_current_loop()`, and the
lazily-constructed default loop already exists) is:

```asm
mov    0x10(%rdi),%rax    # load: explicit_loop_
test   %rax,%rax
je     <default-loop-path>
ret                        # (skipped on the warm/no-explicit-loop path)
<default-loop-path>:
lea    0x18(%rdi),%rax    # address of default_loop_'s storage - no load
cmpb   $0x0,0x80(%rdi)    # load: the optional's has-value byte
jne    <return>
<construct default_loop_ here, once>
<return>:
ret
```

Two more loads (`explicit_loop_`, then the has-value byte), two branches,
a `ret`. **End to end, `current_loop()` costs about 10 instructions and 5
loads across the two function bodies — only 3 of those loads are genuine
pointer chases** (global → platform object → vtable → function pointer);
the other 2 are flag/field reads inside the already-resolved platform
object.

## `.allocator()` added on top

```asm
push   %rax                # reserve a stack slot - needed now that this is a real call, not a tail call
mov    current_instance(%rip),%rdi
mov    (%rdi),%rax
call   *0x40(%rax)          # real call this time: more work follows the return
mov    (%rax),%rax          # load: loop's first member *is* allocator_
pop    %rcx
ret
```

`loop::allocator()` itself costs exactly one more load, nothing else:
`allocator_` is `est::loop`'s first data member, and
`std::pmr::polymorphic_allocator<std::byte>` is just a `memory_resource*`
under the hood - there's no separate virtual accessor to call. The only
overhead `.allocator()` actually adds beyond `current_loop()` alone is the
`push`/`pop` pair, because the tail call above had to become a real `call`
(there's a load to perform on the returned `loop*` afterward).

## The realistic case: `make_promise_future<int>()`

Most code doesn't call `current_loop()`/`.allocator()` directly - it calls
something like `make_promise_future<T>()`, which chains one more virtual
dispatch on top to actually allocate:

```asm
mov  current_instance(%rip),%rdi   # 1: global -> platform object
mov  (%rdi),%rax                   # 2: object -> vtable
call *0x40(%rax)                   # 3: vtable -> get_current_loop_context(), CALL -> loop*
mov  (%rax),%r12                   # 4: loop -> allocator_ (memory_resource*)
mov  (%r12),%rax                   # 5: memory_resource -> its own vtable
...
call *0x10(%rax)                   # 6: vtable -> do_allocate(), CALL -> the actual allocation
```

**Six memory loads and two indirect (virtual) calls before
`do_allocate()`'s own body even starts.** Both calls are monomorphic in
any program that installs exactly one `platform::interface` and uses one
`memory_resource` (the common case) - a modern indirect-branch predictor
should predict both reliably after warmup - but they're real `call`/`ret`
pairs, not inlined; nothing here is free.

## Whole-program devirtualization doesn't help as-is

Clang can, in principle, replace a virtual call with a direct one (or
inline it outright) when it can prove only one concrete type ever reaches
that call site - `-fwhole-program-vtables` at compile and link time is
what asks for it. Tried against this codebase, two results:

- **`-fwhole-program-vtables` alone: no change.** Clang's whole-program
  devirtualization pass stays conservative without proof that no other
  translation unit could define a `platform::interface` subclass - the
  vtables here have ordinary (non-hidden) visibility, so nothing rules
  that out.
- **Combined with `-fvisibility=hidden -fvisibility-inlines-hidden`: it
  triggers.** The generated code gets a compile-time pointer-equality
  check against `&hosted_stdcpp::get_current_loop_context`, and on match,
  inlines that function's body directly - no `call`/`ret` at all for the
  loop lookup.

That sounds strictly better, but wasn't in practice: on the warm path,
the speculatively-devirtualized version was **19 instructions, not
10** - the compiler now has to spill and restore callee-saved registers
around the code path that inlines a call it still can't rule out
(constructing the default loop, which itself calls
`std::pmr::get_default_resource()`), even when that branch isn't taken.
The plain tail-call version above pays none of that: it never needs a
stack frame of its own. Not adopted here - it would also need a
project-wide `-fvisibility=hidden` posture this codebase doesn't
currently take, for a change that didn't measure faster on the path that
matters.

## Follow-up experiment: removing every cached `loop&`/allocator member

The numbers above motivated a second experiment (branch
`current-loop-only-experiment`): if `current_loop()` is this cheap to
reach, what happens if nothing caches it any more - no `future_state<T>`
`loop&` member, no `est::mutex`/`est::counting_event<Mode>` `loop&`
member, no `memory_resource*` stashed alongside a coroutine frame for its
`operator delete` to read back - and every one of those methods resolves
`current_loop()` fresh, at the point of use, instead?

**Codegen cost.** `current_loop()`/`.allocator()`/`platform::instance()`
themselves are unchanged (re-disassembled on this branch - identical to
the listings above) - this experiment didn't touch that mechanism, only
who calls it and how often. What changed is the multiplier: a type like
`est::mutex` used to pay for that lookup chain once, in its constructor,
and store the result; now `lock()`, `unlock()`, `acquire()`, and `~mutex()`
each pay it again, independently, every time they run. The same is true
of `future_state<T>`'s `set_continuation()`, `allocator()`, `then()`,
`complete()`, and `~future_state()`, and of the coroutine frame's own
`operator new`/`operator delete`. None of this is free, even though each
individual lookup is cheap (the ~10-instruction, 2-indirect-call chain
measured above) - a chain of `N` `.then()` calls that used to touch a
cached `loop&` a handful of times now re-resolves `current_loop()` once
per method call across the whole chain.

**Correctness cost - a real regression, not just a documented risk.**
Resolving `current_loop()` fresh instead of caching a reference also
turned a structural guarantee ("this object's loop reference is fixed at
construction and never changes under it") into a live invariant a caller
has to maintain by hand ("the current-loop registration must not change
while this object, or anything it allocated, is still alive"). This
wasn't just a theoretical hazard: it broke this codebase's own
"destroying a loop/mutex/event with a coroutine still pending leaks
nothing" test family outright - `SEGFAULT`/`Subprocess aborted` under
plain `ctest`, from a coroutine frame's `operator delete` resolving
`current_loop()` *after* `make_current_loop()`'s own RAII guard had
already unregistered it (C++'s reverse-destruction-order rule means the
guard, declared after the loop it guards, always unregisters before the
loop's own destructor runs). See [Loop and Timers](Loop-And-Timers.md)'s
"A structural hazard" section for the full mechanism and the fix
(`loop::drain_pending()`, called by the guard before it unregisters) -
closing it required a new public method and a real behavioral change (a
loop's still-pending work is now destroyed as soon as it stops being
`current_loop()`, not only when the loop itself is later destroyed), not
just a documentation update.

**Where this leaves the comparison**: the raw per-lookup cost is small and
was already paid by every caller of the loop-taking design too (whichever
loop a cached `loop&` member held still had to be *reached* at
construction time via this same mechanism). What removing the cache
actually bought is a smaller object (`future_state<T>`/`est::mutex`/
`est::counting_event<Mode>` each shed one member) and no way to pass an
explicit, non-current loop at all - and what it cost is repeated lookups
on every hot-path call instead of one at construction, plus a genuine,
previously-impossible crash that only a design-level fix (not a local
patch) could close.

## The `thread_local` migration

The "Follow-up experiment" above answered "does removing every cached
`loop&` make things worse" with a qualified yes - real per-call cost,
plus a genuine crash. The natural next question: is the *lookup
mechanism itself* (`platform::interface`'s virtual
`get_current_loop_context()`/`set_current_loop_context()`, a global
`current_instance` pointer) the best available, or just the one this
codebase happened to build first? Motivated by a concrete target
(shared-memory, no-MMU multicore, one loop per core - where a plain
global is actively wrong, since every core would fight over the same
slot), this codebase now stores the current loop, its allocator's
`memory_resource*`, and the active `platform::interface*` in
`thread_local` variables instead - `est:util.current_loop`'s own
`execution_context` and `:platform`'s `current_instance`, both
`thread_local` now, and `platform::interface` no longer has
`get_current_loop_context()`/`set_current_loop_context()` at all. See
[Loop and Timers](Loop-And-Timers.md) for the full design; this section
only adds the measured cost, the same way the rest of this page does.

Re-disassembled on the real, migrated code (not a mockup - `examples/probe`'s
`probe_current_loop()`/`probe_loop_allocator()`/`probe_current_allocator()`/
`probe_platform_instance()` call directly into `est::current_loop()`/
`est::current_allocator()`/`est::platform::instance()`):

```asm
; est::current_loop()
mov    %fs:<offset>,%rax   ; one thread-local load
ret
```

```asm
; est::current_allocator() - direct cached resource_ptr, no loop dereference
mov    %fs:<offset>,%rax   ; one thread-local load
ret
```

```asm
; est::platform::instance()
mov    %fs:<offset>,%rax   ; one thread-local load
ret
```

Two instructions, one load, zero indirect branches, zero `call`/`ret` pairs -
for all three. Compare against the numbers earlier on this page: `current_loop()`
alone drops from 3 instructions/1 indirect tail-jump to 2 instructions/0
dispatch; `.allocator()` reached through the loop (`current_loop().allocator()`,
still available, `probe_loop_allocator()`) drops from 7 instructions/1 real
call to 3 instructions/0 dispatch; `current_allocator()` - the new, direct
path most internal callers actually use now - is 2 instructions, cheaper
than even the old `current_loop()` alone was. `platform::instance()` was
already a plain global load before (2 instructions) and stays exactly that
cost with `thread_local` - the per-core isolation is free on this
toolchain/target (x86-64 Linux/glibc, `%fs`-relative addressing), though
that specific equivalence is not guaranteed to hold on every target (see
the caveat below).

The registration side (`est::make_current_loop()`, `est::platform::override_instance()`)
shows the same pattern:

```asm
; est::make_current_loop(loop&) - was 13 instructions incl. 1 virtual call
mov    (%rsi),%rax
mov    %rsi,%fs:<loop_ptr offset>
mov    %rax,%fs:<resource_ptr offset>
mov    %rsi,(%rdi)          ; return the scope_exit guard
ret
```

Five instructions, zero calls - versus 13 instructions and one virtual
call in the pre-migration version. `override_instance()` was already
cheap (a plain global swap, 4 instructions) and stays the same shape,
just `thread_local` now instead of process-global.

**Caveat, repeated from the earlier design discussion and still
unresolved**: this is measured on the host toolchain (glibc TLS via the
`%fs` segment on x86-64 Linux), not the bare-metal, no-MMU target this
change is ultimately for. The win is real *here*, and the mechanism
(read a per-core base pointer, no synchronization) is architecturally
sound for that target in principle - but it depends on that target's own
toolchain implementing `thread_local` similarly cheaply, which has not
been verified.

## Where this fits

See [Loop and Timers](Loop-And-Timers.md) for what `current_loop()` is
and why it's built the way it is; this page only adds what it actually
costs, measured. See [Allocation Patterns](Allocation-Patterns.md) for
the companion question - not how the allocator is *reached*, but how many
allocations each operation performs once it has one.

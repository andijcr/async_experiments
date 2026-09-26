// This ARM LLVM-embedded-toolchain-for-Arm sysroot's own picolibc build
// genuinely never implements __cxa_thread_atexit (confirmed empirically:
// absent from libc.a/libc++.a/libc++abi.a/libunwind.a entirely) - the
// Itanium C++ ABI function the compiler calls to register a destructor
// for a `thread_local` variable that has one (est::detail::
// spawn_exception_hook_storage, est/src/spawn.cppm's own thread_local
// exception hook, is the one that actually needs it here, transitively
// pulled in by anything - platform_rp2040.cppm/serial.cppm included -
// that calls est::spawn()).
//
// __cxa_atexit (registering a destructor for normal *program* exit
// instead of *thread* exit) has the identical signature and *is*
// present in this sysroot - and on this target, the two are equivalent
// in practice: core0 never "exits" as a thread short of the board
// itself losing power, at which point neither callback would ever run
// anyway. Forwarding is exactly what several other bare-metal C++
// runtimes missing a real __cxa_thread_atexit already do for the
// identical reason - not a hack specific to this project.
extern int __cxa_atexit(void (*destructor)(void*), void* arg, void* dso_handle);

int __cxa_thread_atexit(void (*destructor)(void*), void* arg, void* dso_handle) {
  return __cxa_atexit(destructor, arg, dso_handle);
}

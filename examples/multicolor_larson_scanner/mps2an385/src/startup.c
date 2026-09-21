// This board's own Reset_Handler/vector table - not picolibc's own
// crt0.o (link.ld's own top comment has the reasoning for building on
// picolibcpp.ld while keeping this hand-rolled). Everything below the
// vector table (.data/.bss/.tdata/.tbss init, heap/stack) is
// picolibcpp.ld's responsibility; this only does the copy-loops/calls
// its own symbols document as the board's job.
//
// .data/.tdata are one contiguous LOAD image (picolibcpp.ld places
// .tdata immediately after .data, precisely so one copy-loop covers
// both); .tbss/.bss are similarly one contiguous zero-fill range
// (__bss_start == ADDR(.tbss), __bss_end == end of .bss) - see
// picolibcpp.ld's own PROVIDE(__data_end = .)/PROVIDE(__bss_start =
// ADDR(.tbss)) comments for why.

#include <stdint.h>

extern int main(void);
extern unsigned int __stack;

extern unsigned char __data_start[];
extern unsigned char __data_source[];
extern unsigned int __data_size;

extern unsigned char __bss_start[];
extern unsigned int __bss_size;

typedef void (*init_fn)(void);
extern init_fn __preinit_array_start, __preinit_array_end;
extern init_fn __init_array_start, __init_array_end;

// picolibc (libc.a) - __aeabi_read_tp() (called by any thread_local
// access, including libc++abi's own per-"thread" eh_globals every
// throw/typed-catch needs) reads a fixed slot that only _set_tls()
// initializes. __tls_base is picolibcpp.ld's own provided symbol
// (ADDR(.tdata)).
extern void _set_tls(void* tls_block);
extern unsigned char __tls_base[];

void Reset_Handler(void) {
  unsigned char* src = __data_source;
  unsigned char* dst = __data_start;
  for (unsigned int i = 0; i < (unsigned int)&__data_size; ++i) {
    *dst++ = *src++;
  }

  unsigned char* bss = __bss_start;
  for (unsigned int i = 0; i < (unsigned int)&__bss_size; ++i) {
    *bss++ = 0;
  }

  _set_tls(__tls_base);

  // C++ global/static objects with non-trivial constructors - normally
  // __libc_init_array()'s job on a hosted target.
  for (init_fn* fn = &__preinit_array_start; fn < &__preinit_array_end; ++fn) {
    (*fn)();
  }
  for (init_fn* fn = &__init_array_start; fn < &__init_array_end; ++fn) {
    (*fn)();
  }

  main();
  for (;;) {
  }
}

void Default_Handler(void) {
  for (;;) {
  }
}

// Defined in estpico's own platform_mps2an385.cppm (extern "C", so these
// declarations and those definitions agree on linkage) - this backend's
// own clock/UART-TX state belongs to that module, not to this file, the
// same split _exit()/gettimeofday() below already have with
// picolibc/libc++.
void SysTick_Handler(void);
void UART0_TX_Handler(void); // IRQ1 - confirmed empirically, see that module's own comment

// picolibcpp.ld's .boot_flash output section collects .text.init.enter
// (among others) at ORIGIN(boot_flash) - exactly where a Cortex-M
// expects its vector table (word0 = initial SP, word1 = reset vector).
__attribute__((section(".text.init.enter"))) void (*const vector_table[16 + 2])(void) = {
    (void (*)(void))&__stack,
    Reset_Handler,
    Default_Handler, // NMI
    Default_Handler, // HardFault
    Default_Handler, // MemManage
    Default_Handler, // BusFault
    Default_Handler, // UsageFault
    0,
    0,
    0,
    0,               // reserved
    Default_Handler, // SVCall
    Default_Handler, // DebugMonitor
    0,               // reserved
    Default_Handler, // PendSV
    SysTick_Handler,
    Default_Handler,  // IRQ0 - UART0 RX (unused)
    UART0_TX_Handler, // IRQ1
};

// ARM semihosting SYS_EXIT_EXTENDED (op 0x20) - the only semihosting
// exit operation that propagates an arbitrary host process exit code on
// AArch32 (issue #123 Finding 1); every program in this project reaches
// _exit() through this, whether via a normal return from main(), an
// abort()/raise() chain, or an unhandled exception.
static void semihost_exit(int code) {
  static uint32_t block[2];
  block[0] = 0x20026u; // ADP_Stopped_ApplicationExit
  block[1] = (uint32_t)code;
  register uint32_t r0 __asm__("r0") = 0x20u;
  register uint32_t r1 __asm__("r1") = (uint32_t)block;
  __asm__ volatile("bkpt 0xab" : : "r"(r0), "r"(r1) : "memory");
  for (;;) {
  }
}

void _exit(int code) { semihost_exit(code); }

// A thread_local with a non-trivial destructor (est/src/spawn.cppm's own
// exception hook) unconditionally emits a call to this to register its
// destructor for later, real threading or not - picolibc doesn't provide
// it at all on this freestanding, no-threads target (_LIBCPP_HAS_NO_THREADS,
// cmake/toolchain-mps2an385.cmake's own EST_NO_THREADS comment). This
// "thread" (the one core) never exits in a way that would ever run a
// registered destructor - this firmware only ever terminates via
// semihost_exit() above, which calls straight into QEMU's monitor and
// never returns - so accepting and discarding the registration is a
// correct no-op, not a stub papering over a real gap.
int __cxa_thread_atexit(void (*destructor)(void*), void* object, void* dso_symbol) {
  (void)destructor;
  (void)object;
  (void)dso_symbol;
  return 0;
}

// __libcpp_verbose_abort() (libc++.a) wants a real FILE* stderr from
// picolibc's stdio to fprintf a diagnostic before aborting - not wired
// up in this project (no stdio backend at all, est's own platform
// diagnostics go through estpico's UART writer instead), just enough of
// a symbol to link; never actually dereferenced unless verbose_abort's
// own path runs.
void* stderr = (void*)0;

// getchar() (picolibc) wants a FILE* stdin - pulled in transitively by
// Catch2/Clara's linked object set, never actually called (Session::run()
// never reads from stdin here).
void* stdin = (void*)0;

// picolibc's time() implementation calls this to compute wall-clock time
// - Catch2's own std::time(nullptr)-based random-seed fallback
// (third_party/catch2-baremetal.patch) needs *a* value, not a correct
// one; this board has no RTC wired up.
struct timeval;
int gettimeofday(struct timeval* tv, void* tz) {
  (void)tv;
  (void)tz;
  return -1;
}

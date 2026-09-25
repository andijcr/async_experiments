// env.js_* imports - estwasm/src/platform_wasm.cppm's own JS half.
// waitBuffer is a tiny, otherwise-unused SharedArrayBuffer: nothing ever
// writes to index 0, so every Atomics.wait() below deterministically
// times out at the requested deadline instead of ever being woken early -
// a real, synchronous sleep, not a busy-loop. Shared between main.js and
// worker.js (only worker.js's own instantiation ever actually calls
// sleep_until()/now() through est::loop - see platform_wasm.cppm's own
// sleep_until() comment - but every import has to resolve at
// instantiation time on both sides regardless).
export function makeEnvShim(memoryRef, waitBuffer, label) {
  const decoder = new TextDecoder();
  const waitView = new Int32Array(waitBuffer);

  return {
    memory: memoryRef,
    js_now_ms() {
      return performance.now();
    },
    js_sleep_until_ms(deadlineMs) {
      const timeout = Math.max(0, deadlineMs - performance.now());
      // Atomics.wait is Worker-only - throws TypeError on the main
      // thread. That's deliberate: platform_wasm::interruptible_sleep_until()
      // is only ever called from the Worker's own est::loop::run(), never
      // from the main thread's instantiation.
      Atomics.wait(waitView, 0, 0, timeout);
    },
    js_wake() {
      // Atomics.notify on the identical index js_sleep_until_ms() above
      // waits on - never touches the value there (still always 0, per
      // this file's own top comment), only wakes whatever's currently
      // blocked. A no-op if nothing is waiting - not an error, callable
      // from the main thread or another Worker at any time.
      Atomics.notify(waitView, 0);
    },
    js_random_u32() {
      const buf = new Uint32Array(1);
      crypto.getRandomValues(buf);
      return buf[0];
    },
    js_report(ptr, len) {
      const text = decoder.decode(new Uint8Array(memoryRef.buffer, ptr, len));
      console.log(`[${label}]`, text);
    },
    js_worker_ready(redPtr, greenPtr, bluePtr, width) {
      if (typeof globalThis.__onWorkerReady === 'function') {
        globalThis.__onWorkerReady(redPtr, greenPtr, bluePtr, width);
      }
    },
  };
}

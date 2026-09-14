// Node-side twin of ../env_shim.js/../wasi_shim.js - same shapes, same
// reasoning (see those files' own comments), just using Node's globals
// (performance, crypto, TextDecoder are all available without an import
// on Node 18+) instead of a browser's. Not literally shared with the
// production browser files: node:worker_threads' Worker API differs
// enough from the browser's (workerData/parentPort vs.
// self.onmessage/postMessage) that the two Worker-side entry points
// can't be the same file anyway, so duplicating this much smaller
// env/wasi glue here rather than factoring out a cross-runtime
// abstraction for a one-file CI smoke test.

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
      Atomics.wait(waitView, 0, 0, timeout);
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

export function makeWasiShim(memoryRef) {
  const decoder = new TextDecoder();
  const dv = () => new DataView(memoryRef.buffer);

  return {
    environ_sizes_get(environcPtr, environBufSizePtr) {
      dv().setUint32(environcPtr, 0, true);
      dv().setUint32(environBufSizePtr, 0, true);
      return 0;
    },
    environ_get() {
      return 0;
    },
    clock_time_get(_clockId, _precision, timePtr) {
      const ns = BigInt(Math.round(performance.now() * 1e6));
      dv().setBigUint64(timePtr, ns, true);
      return 0;
    },
    fd_close() {
      return 0;
    },
    fd_prestat_get() {
      return 8;
    },
    fd_prestat_dir_name() {
      return 8;
    },
    fd_seek() {
      return 8;
    },
    fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
      let written = 0;
      let text = '';
      const view = dv();
      for (let i = 0; i < iovsLen; i++) {
        const base = iovsPtr + i * 8;
        const ptr = view.getUint32(base, true);
        const len = view.getUint32(base + 4, true);
        text += decoder.decode(new Uint8Array(memoryRef.buffer, ptr, len));
        written += len;
      }
      if (text.length > 0) {
        (fd === 2 ? console.error : console.log)(`[wasm fd_write ${fd}]`, text);
      }
      view.setUint32(nwrittenPtr, written, true);
      return 0;
    },
    proc_exit(code) {
      throw new Error(`wasm module called proc_exit(${code})`);
    },
    sched_yield() {
      return 0;
    },
  };
}

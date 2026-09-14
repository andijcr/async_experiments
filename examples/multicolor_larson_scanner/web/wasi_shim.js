// Minimal wasi_snapshot_preview1 shim - just enough for wasi-libc's own
// crt (crt1-reactor.o) startup path and est/larson_scanner's actual
// runtime needs (sched_yield() from std::this_thread::yield(),
// clock_time_get() from libc++'s own internals) to be satisfied. No real
// file/env access - this app never does I/O through libc, only through
// env.js_* (see ../../../estwasm/src/platform_wasm.cppm). Ported from the
// Node.js version used to verify the design end-to-end (docs/PLAN.md's
// "Issue #99" entry) - same behavior, TextDecoder instead of Node's
// Buffer.
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
      return 8; // EBADF - "no preopens", stops wasi-libc's own scan loop
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

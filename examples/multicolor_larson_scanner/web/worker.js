import { makeEnvShim } from './env_shim.js';
import { makeWasiShim } from './wasi_shim.js';

// The Worker side of the Worker/main-thread split (docs/PLAN.md's "Issue
// #99" entry has the full design). Receives one message from main.js -
// the compiled WebAssembly.Module (compiled once there, not re-compiled
// here), the shared WebAssembly.Memory, the tiny Atomics.wait()
// scratch buffer, and the initial app_config knobs - instantiates,
// installs estwasm's platform backend implicitly (boot()'s own job, in
// wasm_exports.cpp), and calls boot(), which blocks this Worker forever.

self.onmessage = async (event) => {
  const { module, memory, waitBuffer, width, speed, decay } = event.data;

  globalThis.__onWorkerReady = (redPtr, greenPtr, bluePtr, w) => {
    self.postMessage({ type: 'ready', redPtr, greenPtr, bluePtr, width: w });
  };

  const instance = await WebAssembly.instantiate(module, {
    env: makeEnvShim(memory, waitBuffer, 'worker'),
    wasi_snapshot_preview1: makeWasiShim(memory),
  });

  // Deliberately NOT calling instance.exports._initialize() here: the
  // module's shared-memory data/ctor init is guarded by an atomic
  // compare-and-swap whose "not first" branch is a literal `unreachable`
  // trap, not a wait - meaning exactly one instantiation (the primary,
  // done in main.js before this Worker even starts) is meant to call
  // it, and every other instantiation sharing that memory must skip it
  // entirely (confirmed by disassembly + a real two-instantiation Node
  // test - docs/PLAN.md's "Issue #99" entry).

  // Blocks forever - est::loop::run(), via platform_wasm::sleep_until()'s
  // real Atomics.wait(). Everything after this call is dead code,
  // matching app.loop()'s own documented contract.
  instance.exports.boot(width, speed, decay);
};

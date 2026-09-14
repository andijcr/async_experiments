// The Worker side of smoke_test.mjs's own two-instantiation check - a
// node:worker_threads analog of ../worker.js (the real browser Worker
// script), reusing the same shim shapes but through Node's own APIs
// (workerData/parentPort instead of self.onmessage/postMessage) since
// the two Worker APIs aren't drop-in compatible. See smoke_test.mjs's
// own top comment for what this is checking and why.
import { parentPort, workerData } from 'node:worker_threads';
import { makeEnvShim, makeWasiShim } from './shim.mjs';

const { module, memory, waitBuffer, width, speed, decay } = workerData;

globalThis.__onWorkerReady = (redPtr, greenPtr, bluePtr, w) => {
  parentPort.postMessage({ type: 'ready', redPtr, greenPtr, bluePtr, width: w });
};

try {
  const instance = await WebAssembly.instantiate(module, {
    env: makeEnvShim(memory, waitBuffer, 'worker'),
    wasi_snapshot_preview1: makeWasiShim(memory),
  });

  // Deliberately not calling instance.exports._initialize() - see
  // ../worker.js's own comment (same reasoning, same module): the
  // shared-memory data/ctor init is a one-shot atomic guard whose
  // "already done" branch is an `unreachable` trap, not a wait, so only
  // the primary instantiation (smoke_test.mjs's own, before this Worker
  // starts) may call it.
  parentPort.postMessage({ type: 'booting' });
  // Blocks forever - est::loop::run(), via a real Atomics.wait().
  instance.exports.boot(width, speed, decay);
} catch (err) {
  parentPort.postMessage({ type: 'error', message: String((err && err.stack) || err) });
}

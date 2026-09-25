// Headless smoke test for the wasm32 build (CI's own "wasm" job,
// .github/workflows/ci.yml): a real node:worker_threads Worker and this
// main script each instantiate the compiled module, sharing one
// WebAssembly.Memory, mirroring the browser design end to end (Worker
// boots a real est::loop and blocks on a real Atomics.wait(), the main
// side pushes a command via a genuine synchronous push_command() call,
// not postMessage) - this is what actually caught the two real
// integration bugs a build-only check wouldn't have (an
// --import-memory linker-flag gap, and the _initialize()-must-be-
// primary-only single-init protocol). See docs/PLAN.md's "Issue #99"
// entry for the full story.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker } from 'node:worker_threads';
import { makeEnvShim, makeWasiShim } from './shim.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = process.argv[2];
if (!wasmPath) {
  console.error('usage: node smoke_test.mjs <path-to-larson_scanner_wasm>');
  process.exit(2);
}

function fail(message) {
  console.error('FAIL:', message);
  process.exit(1);
}

const wasmBytes = fs.readFileSync(wasmPath);
const module = await WebAssembly.compile(wasmBytes);

// Import-section check (this repo's own stand-in for wasm-objdump/wasm2wat,
// neither of which is installed in the devenv image): only the small,
// deliberate env.js_*/memory imports this design actually adds, plus
// wasi-libc's own crt boilerplate - nothing beyond what M0/M0.5 already
// established as necessary. A new, unexpected import here usually means
// something pulled in real WASI I/O (file/network/env access) this
// sandboxed browser app was never meant to need.
const EXPECTED_IMPORTS = new Set([
  'env.memory',
  'env.js_now_ms',
  'env.js_sleep_until_ms',
  'env.js_wake',
  'env.js_random_u32',
  'env.js_report',
  'env.js_worker_ready',
  'wasi_snapshot_preview1.environ_get',
  'wasi_snapshot_preview1.environ_sizes_get',
  'wasi_snapshot_preview1.clock_time_get',
  'wasi_snapshot_preview1.fd_close',
  'wasi_snapshot_preview1.fd_prestat_get',
  'wasi_snapshot_preview1.fd_prestat_dir_name',
  'wasi_snapshot_preview1.fd_seek',
  'wasi_snapshot_preview1.fd_write',
  'wasi_snapshot_preview1.proc_exit',
  'wasi_snapshot_preview1.sched_yield',
]);
const actualImports = WebAssembly.Module.imports(module).map((i) => `${i.module}.${i.name}`);
const unexpected = actualImports.filter((i) => !EXPECTED_IMPORTS.has(i));
if (unexpected.length > 0) {
  fail(`unexpected wasm imports: ${unexpected.join(', ')}`);
}
console.log('import-section check: OK', actualImports);

const memory = new WebAssembly.Memory({ initial: 16, maximum: 256, shared: true });
const waitBuffer = new SharedArrayBuffer(4);

const mainInstance = await WebAssembly.instantiate(module, {
  env: makeEnvShim(memory, waitBuffer, 'main'),
  wasi_snapshot_preview1: makeWasiShim(memory),
});
mainInstance.exports._initialize();

const ready = await new Promise((resolve, reject) => {
  const worker = new Worker(path.join(__dirname, 'smoke_worker.mjs'), {
    workerData: { module, memory, waitBuffer, width: 8, speed: 20.0, decay: 0.01 },
  });
  worker.on('message', (msg) => {
    if (msg.type === 'ready') {
      resolve({ worker, ...msg });
    } else if (msg.type === 'error') {
      reject(new Error(`worker error: ${msg.message}`));
    }
  });
  worker.on('error', reject);
});

const { worker, redPtr, greenPtr, bluePtr, width } = ready;
console.log('worker ready:', { redPtr, greenPtr, bluePtr, width });

const readChannel = (ptr) => Array.from(new Float32Array(memory.buffer, ptr, width));
const before = JSON.stringify({
  red: readChannel(redPtr),
  green: readChannel(greenPtr),
  blue: readChannel(bluePtr),
});

// Real C++ push_command(), on this main script's own call stack -
// reaching the Worker's app object via shared memory, not postMessage.
mainInstance.exports.push_command(/* channel=red */ 0, /* param=speed */ 0, 40.0);
mainInstance.exports.push_command(/* channel=all */ 3, /* param=decay */ 1, 0.5);

await new Promise((resolve) => setTimeout(resolve, 200));

const after = JSON.stringify({
  red: readChannel(redPtr),
  green: readChannel(greenPtr),
  blue: readChannel(bluePtr),
});

await worker.terminate();

if (before === after) {
  fail('led_buffer intensities did not change - worker loop is not ticking, or shared memory is not visible cross-instance');
}

console.log('PASS: worker loop ticked and shared memory is visible cross-instance');
process.exit(0);

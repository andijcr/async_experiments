import { makeEnvShim } from './env_shim.js';
import { makeWasiShim } from './wasi_shim.js';

// Main-thread driver: creates the one shared WebAssembly.Memory both
// instantiations use, boots the Worker, wires sliders to push_command(),
// and paints the LED strip on its own requestAnimationFrame cadence by
// reading led_buffer's intensities directly out of shared memory - no
// wasm call needed per frame (docs/PLAN.md's "Issue #99" entry has the
// full design and why).

const STRIP_WIDTH = 40;
const INITIAL_SPEED = 20.0;
const INITIAL_DECAY = 0.01;

// Must match cmake/toolchain-wasm32.cmake's own
// -Wl,--initial-memory=1048576 -Wl,--max-memory=16777216 (pages are
// 64KiB: 1048576/65536 = 16, 16777216/65536 = 256).
const MEMORY_INITIAL_PAGES = 16;
const MEMORY_MAX_PAGES = 256;

const ChannelSelector = { red: 0, green: 1, blue: 2, all: 3 };
const ParamKind = { speed: 0, decay: 1 };

const ledStrip = document.getElementById('led-strip');
const statusEl = document.getElementById('status');
const controls = document.querySelectorAll('[data-channel]');

const leds = [];
for (let i = 0; i < STRIP_WIDTH; i++) {
  const led = document.createElement('div');
  led.className = 'led';
  ledStrip.appendChild(led);
  leds.push(led);
}

function setStatus(text) {
  statusEl.textContent = text;
}

async function main() {
  setStatus('loading…');

  const memory = new WebAssembly.Memory({
    initial: MEMORY_INITIAL_PAGES,
    maximum: MEMORY_MAX_PAGES,
    shared: true,
  });
  const waitBuffer = new SharedArrayBuffer(4);

  const response = await fetch('larson_scanner.wasm');
  const module = await WebAssembly.compileStreaming(response);

  // Main-thread-side instantiation: never calls boot(), only
  // push_command() (below) - but still needs every declared import
  // resolved at instantiation time, and is the one instantiation that
  // *does* call _initialize() (see worker.js's own comment on why the
  // Worker's instantiation must not).
  const mainInstance = await WebAssembly.instantiate(module, {
    env: makeEnvShim(memory, waitBuffer, 'main'),
    wasi_snapshot_preview1: makeWasiShim(memory),
  });
  mainInstance.exports._initialize();

  const ready = new Promise((resolve) => {
    globalThis.__onWorkerReady = (redPtr, greenPtr, bluePtr, width) => {
      resolve({ redPtr, greenPtr, bluePtr, width });
    };
  });

  const worker = new Worker('worker.js', { type: 'module' });
  worker.onmessage = (event) => {
    if (event.data.type === 'ready') {
      globalThis.__onWorkerReady(
        event.data.redPtr,
        event.data.greenPtr,
        event.data.bluePtr,
        event.data.width,
      );
    }
  };
  worker.postMessage({
    module,
    memory,
    waitBuffer,
    width: STRIP_WIDTH,
    speed: INITIAL_SPEED,
    decay: INITIAL_DECAY,
  });

  const { redPtr, greenPtr, bluePtr, width } = await ready;
  setStatus('running');

  for (const control of controls) {
    control.disabled = false;
    control.addEventListener('input', () => {
      const channel = ChannelSelector[control.dataset.channel];
      const param = ParamKind[control.dataset.param];
      mainInstance.exports.push_command(channel, param, Number(control.value));
    });
  }

  const red = new Float32Array(memory.buffer, redPtr, width);
  const green = new Float32Array(memory.buffer, greenPtr, width);
  const blue = new Float32Array(memory.buffer, bluePtr, width);

  function paint() {
    for (let i = 0; i < width; i++) {
      const r = red[i];
      const g = green[i];
      const b = blue[i];
      const glow = Math.max(r, g, b);
      const style = leds[i].style;
      style.setProperty('--rgb', `${Math.round(r * 255)} ${Math.round(g * 255)} ${Math.round(b * 255)}`);
      style.setProperty('--glow', glow.toFixed(3));
    }
    requestAnimationFrame(paint);
  }
  requestAnimationFrame(paint);
}

main().catch((err) => {
  console.error(err);
  setStatus(`error: ${err.message}`);
});

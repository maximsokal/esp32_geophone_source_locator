/**
 * The firmware's source-localization run, compiled to WebAssembly (tools/build_localization_wasm.py). The page hands it a
 * recording, the session's frozen geometry and the calculation settings; it returns the pick record and the run records in
 * the schema the device itself stores, so the result can be sent to the ESP32 and written there unchanged.
 */

export interface KernelExports {
  memory: WebAssembly.Memory;
  lw_alloc(bytes: number): number;
  lw_free(pointer: number): void;
  lw_reset(): void;
  lw_frame_size(): number;
  lw_frame_timestamp_offset(): number;
  lw_frame_channels_offset(): number;
  lw_set_recording(frames: number, sourceIndices: number, gapBefore: number, count: number, sampleRateHz: number): number;
  lw_set_geometry_json(json: number): number;
  lw_check_config(configJson: number): number;
  lw_run(objectId: number, sessionId: number, configJson: number, computedAtMs: bigint): number;
  lw_result_json(): number;
  lw_record_count(): number;
  lw_channel_health_flat(): number;
  lw_channel_health_identical(): number;
}

/** A recording as the device stores it: four channels in microvolts, the source index of every frame and the holes. */
export interface KernelRecording {
  sampleRateHz: number;
  /** Microseconds since the recording's start, one per frame. */
  timestampsUs: ArrayLike<number>;
  /** frame-major, 4 channels per frame. */
  channelUv: Float32Array;
  /** ADC conversion counter of every frame: a jump marks lost conversions. */
  sourceIndices: ArrayLike<number | bigint>;
  /** 1 on the first frame after a hole, else 0. */
  gapBefore: Uint8Array;
}

export interface KernelResult {
  /** The run's error code: 0 is ESP_OK. */
  status: number;
  /** Where a non-zero status came from: the settings were refused before the run, or the run itself stopped. */
  phase: 'settings' | 'run';
  picks: unknown | null;
  runs: unknown[];
  channelHealth: { flatChannelMask: number; identicalPairMask: number };
}

const CHANNELS = 4;
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

/** The calculation only needs a clock, an allocator and a few no-op system calls; anything else is "not supported". */
function wasiImports(module: WebAssembly.Module, getMemory: () => WebAssembly.Memory): WebAssembly.Imports {
  const wasi: Record<string, (...args: number[]) => number | void> = {};
  for (const entry of WebAssembly.Module.imports(module)) {
    if (entry.kind !== 'function') continue;
    wasi[entry.name] = () => 52; // ENOSYS
  }
  wasi.proc_exit = (code) => { throw new Error(`kernel exited with ${code}`); };
  wasi.fd_write = (_fd, _iovs, _count, written) => {
    new DataView(getMemory().buffer).setUint32(written, 0, true);
    return 0;
  };
  wasi.environ_sizes_get = (countPointer, sizePointer) => {
    const view = new DataView(getMemory().buffer);
    view.setUint32(countPointer, 0, true);
    view.setUint32(sizePointer, 0, true);
    return 0;
  };
  wasi.environ_get = () => 0;
  wasi.args_sizes_get = wasi.environ_sizes_get;
  wasi.args_get = () => 0;
  // The C library probes pre-opened directories at start-up and stops at the first "bad descriptor".
  wasi.fd_prestat_get = () => 8; // EBADF
  wasi.fd_prestat_dir_name = () => 8;
  wasi.clock_time_get = (_id, _precision, out) => {
    new DataView(getMemory().buffer).setBigUint64(out, BigInt(Math.trunc(Date.now())) * 1_000_000n, true);
    return 0;
  };
  wasi.random_get = (pointer, length) => {
    crypto.getRandomValues(new Uint8Array(getMemory().buffer, pointer, length));
    return 0;
  };
  return { wasi_snapshot_preview1: wasi };
}

export class LocalizationKernel {
  private constructor(private readonly exports: KernelExports) {}

  static async load(wasm: BufferSource): Promise<LocalizationKernel> {
    const module = await WebAssembly.compile(wasm);
    let memory: WebAssembly.Memory | null = null;
    const instance = await WebAssembly.instantiate(module, wasiImports(module, () => memory!));
    const exports = instance.exports as unknown as KernelExports & { _initialize?: () => void };
    memory = exports.memory;
    exports._initialize?.();
    return new LocalizationKernel(exports);
  }

  private string(text: string): number {
    const bytes = textEncoder.encode(text);
    const pointer = this.exports.lw_alloc(bytes.length + 1);
    const view = new Uint8Array(this.exports.memory.buffer, pointer, bytes.length + 1);
    view.set(bytes);
    view[bytes.length] = 0;
    return pointer;
  }

  private readString(pointer: number): string {
    const bytes = new Uint8Array(this.exports.memory.buffer);
    let end = pointer;
    while (bytes[end] !== 0) end += 1;
    return textDecoder.decode(bytes.subarray(pointer, end));
  }

  /** 0 when the firmware accepts these calculation settings, else its error code. */
  checkConfig(configJson: string): number {
    const pointer = this.string(configJson);
    try {
      return this.exports.lw_check_config(pointer);
    } finally {
      this.exports.lw_free(pointer);
    }
  }

  /**
   * Runs the firmware's calculation. `geometryJson` is the session's JSON object with the frozen "geometry" the device
   * keeps; `configJson` the calculation settings as the device stores them for the object.
   */
  run(input: {
    objectId: string;
    sessionId: string;
    recording: KernelRecording;
    geometryJson: string;
    configJson: string;
    computedAtMs: number;
  }): KernelResult {
    const { recording } = input;
    const count = recording.gapBefore.length;
    if (count === 0 || recording.channelUv.length !== count * CHANNELS || recording.timestampsUs.length !== count ||
        recording.sourceIndices.length !== count) {
      throw new Error('the recording arrays disagree on the number of frames');
    }
    // The settings first, on their own: "error 258" from a refused setting and from the run are different problems.
    const settingsStatus = this.checkConfig(input.configJson);
    if (settingsStatus !== 0) {
      return { status: settingsStatus, phase: 'settings', picks: null, runs: [], channelHealth: { flatChannelMask: 0, identicalPairMask: 0 } };
    }
    const x = this.exports;
    x.lw_reset();
    const frameSize = x.lw_frame_size();
    const timestampOffset = x.lw_frame_timestamp_offset();
    const channelsOffset = x.lw_frame_channels_offset();
    const framesPointer = x.lw_alloc(count * frameSize);
    const indicesPointer = x.lw_alloc(count * 8);
    const gapPointer = x.lw_alloc(count);
    // Memory may have grown while allocating: take the views only now.
    const view = new DataView(x.memory.buffer);
    for (let i = 0; i < count; i += 1) {
      const frame = framesPointer + i * frameSize;
      view.setBigInt64(frame + timestampOffset, BigInt(Math.trunc(Number(recording.timestampsUs[i]))), true);
      for (let channel = 0; channel < CHANNELS; channel += 1) {
        view.setFloat32(frame + channelsOffset + channel * 4, recording.channelUv[i * CHANNELS + channel], true);
      }
      view.setBigUint64(indicesPointer + i * 8, BigInt(recording.sourceIndices[i]), true);
    }
    new Uint8Array(x.memory.buffer, gapPointer, count).set(recording.gapBefore);
    const setStatus = x.lw_set_recording(framesPointer, indicesPointer, gapPointer, count, recording.sampleRateHz);
    if (setStatus !== 0) throw new Error(`the kernel refused the recording (${setStatus})`);

    const strings = [input.geometryJson, input.objectId, input.sessionId, input.configJson].map((text) => this.string(text));
    try {
      const geometryStatus = x.lw_set_geometry_json(strings[0]);
      if (geometryStatus !== 0) throw new Error(`the kernel refused the geometry (${geometryStatus})`);
      const status = x.lw_run(strings[1], strings[2], strings[3], BigInt(Math.trunc(input.computedAtMs)));
      const parsed = JSON.parse(this.readString(x.lw_result_json())) as { picks: unknown | null; runs: unknown[] };
      return {
        status,
        phase: 'run',
        picks: parsed.picks,
        runs: parsed.runs,
        channelHealth: { flatChannelMask: x.lw_channel_health_flat(), identicalPairMask: x.lw_channel_health_identical() },
      };
    } finally {
      for (const pointer of strings) x.lw_free(pointer);
    }
  }
}

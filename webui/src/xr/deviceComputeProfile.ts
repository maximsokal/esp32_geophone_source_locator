export type DeviceComputeTier = 'limited' | 'balanced' | 'high';

export interface DeviceComputeSignals {
  reportedLogicalProcessors: number;
  /** Coarse browser RAM hint (Navigator.deviceMemory), absent outside Chromium. */
  reportedDeviceMemoryGiB?: number | null;
  workerApi: boolean;
  crossOriginIsolated: boolean;
  sharedArrayBufferApi: boolean;
  wasmApi: boolean;
  wasmSharedMemory: boolean;
  webGpuApi: boolean;
  webGpuAdapter: boolean;
}

export interface DeviceComputeProfile extends DeviceComputeSignals {
  /** Logical processors deliberately left for the browser/UI/XR compositor. */
  reservedLogicalProcessors: number;
  /** Maximum simultaneously heavy CPU workers while an XR session is live. */
  liveWorkerBudget: number;
  /** Maximum CPU workers recommended after XR has stopped. */
  postScanWorkerBudget: number;
  /** Conservative post-scan working-set budget, independent of the CPU hint. */
  postScanMemoryBudgetBytes: number;
  sharedMemory: boolean;
  wasmThreads: boolean;
  webGpu: boolean;
  tier: DeviceComputeTier;
}

interface NavigatorComputeLike {
  hardwareConcurrency?: number;
  deviceMemory?: number;
  gpu?: {
    requestAdapter(options?: { powerPreference?: 'low-power' | 'high-performance' }): Promise<unknown | null>;
  };
}

interface SharedArrayBufferConstructorLike {
  new (byteLength: number): object;
}

interface GlobalComputeLike {
  crossOriginIsolated?: boolean;
  Worker?: unknown;
  SharedArrayBuffer?: SharedArrayBufferConstructorLike;
  WebAssembly?: typeof WebAssembly;
}

const MAX_REPORTED_LOGICAL_PROCESSORS = 32;
const MAX_LIVE_HEAVY_WORKERS = 2;
const MAX_POST_SCAN_WORKERS = 6;
const MIB = 1024 * 1024;

function postScanMemoryBudgetBytes(deviceMemoryGiB: number | null | undefined, logicalProcessors: number): number {
  // deviceMemory is rounded, optional, and says nothing about browser pressure.
  // Bound it again by the CPU working-set hint; when RAM is unavailable, use a
  // conservative 128 MiB default before applying that CPU ceiling.
  const reportedMemoryBudget = deviceMemoryGiB === null || deviceMemoryGiB === undefined ||
      !Number.isFinite(deviceMemoryGiB) || deviceMemoryGiB <= 0
    ? 128 * MIB
    : Math.max(64 * MIB, Math.min(768 * MIB, Math.floor(deviceMemoryGiB * 1024 * MIB * 0.15)));
  const cpuWorkingSetBudget = Math.max(96 * MIB, Math.min(768 * MIB, logicalProcessors * 96 * MIB));
  return Math.min(reportedMemoryBudget, cpuWorkingSetBudget);
}

function clampInteger(value: number, minimum: number, maximum: number): number {
  if (!Number.isFinite(value)) return minimum;
  return Math.max(minimum, Math.min(maximum, Math.floor(value)));
}

/**
 * Convert browser-reported capability hints into a conservative mobile budget.
 *
 * hardwareConcurrency is intentionally treated as a scheduling hint, never as
 * physical-core topology. Browsers may cap it for privacy and heterogeneous ARM
 * P/E cores cannot be pinned from page JavaScript. Live XR reserves headroom for
 * the main thread, browser internals and compositor; post-scan work may use more
 * workers after camera/depth rendering has stopped.
 */
export function deriveDeviceComputeProfile(signals: DeviceComputeSignals): DeviceComputeProfile {
  const logical = clampInteger(signals.reportedLogicalProcessors, 1, MAX_REPORTED_LOGICAL_PROCESSORS);
  const reservedLogicalProcessors = logical <= 1 ? 0 : logical >= 8 ? 2 : 1;
  const cpuWorkerCapacity = Math.max(1, logical - reservedLogicalProcessors);
  const liveWorkerBudget = signals.workerApi
    ? Math.min(MAX_LIVE_HEAVY_WORKERS, cpuWorkerCapacity)
    : 0;
  const postScanWorkerBudget = signals.workerApi
    ? Math.min(MAX_POST_SCAN_WORKERS, cpuWorkerCapacity)
    : 0;
  const sharedMemory = signals.crossOriginIsolated && signals.sharedArrayBufferApi;
  const wasmThreads = sharedMemory && signals.wasmApi && signals.wasmSharedMemory;
  const webGpu = signals.webGpuApi && signals.webGpuAdapter;
  const tier: DeviceComputeTier = !signals.workerApi || logical <= 2
    ? 'limited'
    : logical >= 8
      ? 'high'
      : 'balanced';

  return {
    ...signals,
    reportedLogicalProcessors: logical,
    reportedDeviceMemoryGiB: signals.reportedDeviceMemoryGiB ?? null,
    reservedLogicalProcessors,
    liveWorkerBudget,
    postScanWorkerBudget,
    postScanMemoryBudgetBytes: postScanMemoryBudgetBytes(signals.reportedDeviceMemoryGiB, logical),
    sharedMemory,
    wasmThreads,
    webGpu,
    tier,
  };
}

function detectWasmSharedMemory(globalScope: GlobalComputeLike): boolean {
  if (
    globalScope.crossOriginIsolated !== true ||
    globalScope.SharedArrayBuffer === undefined ||
    globalScope.WebAssembly === undefined
  ) return false;
  try {
    const memory = new globalScope.WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
    return memory.buffer instanceof globalScope.SharedArrayBuffer;
  } catch {
    return false;
  }
}

/**
 * Probe compute capabilities before an XR session starts. No benchmark is run:
 * warming every core before camera start would itself create heat and bias the
 * result. Runtime frame-pressure telemetry remains authoritative after start.
 */
export async function probeDeviceComputeProfile(): Promise<DeviceComputeProfile> {
  const navigatorLike: NavigatorComputeLike = typeof navigator === 'undefined'
    ? {}
    : navigator as NavigatorComputeLike;
  const globalScope: GlobalComputeLike = globalThis as GlobalComputeLike;
  const reportedLogicalProcessors = clampInteger(
    Number(navigatorLike.hardwareConcurrency ?? 1),
    1,
    MAX_REPORTED_LOGICAL_PROCESSORS,
  );
  const reportedDeviceMemoryGiB = Number.isFinite(navigatorLike.deviceMemory) &&
      (navigatorLike.deviceMemory ?? 0) > 0
    ? navigatorLike.deviceMemory!
    : null;
  const workerApi = typeof globalScope.Worker !== 'undefined';
  const crossOriginIsolated = globalScope.crossOriginIsolated === true;
  const sharedArrayBufferApi = globalScope.SharedArrayBuffer !== undefined;
  const wasmApi = globalScope.WebAssembly !== undefined;
  const wasmSharedMemory = detectWasmSharedMemory(globalScope);
  const webGpuApi = navigatorLike.gpu !== undefined && typeof navigatorLike.gpu.requestAdapter === 'function';
  let webGpuAdapter = false;
  if (webGpuApi) {
    try {
      webGpuAdapter = (await navigatorLike.gpu!.requestAdapter({ powerPreference: 'high-performance' })) !== null;
    } catch {
      webGpuAdapter = false;
    }
  }

  return deriveDeviceComputeProfile({
    reportedLogicalProcessors,
    reportedDeviceMemoryGiB,
    workerApi,
    crossOriginIsolated,
    sharedArrayBufferApi,
    wasmApi,
    wasmSharedMemory,
    webGpuApi,
    webGpuAdapter,
  });
}

/**
 * Module evaluation starts the non-invasive capability negotiation immediately,
 * so worker budgets are ready by the time the user presses Start scan. Static
 * imports of scanner clients make this run during normal WebUI bootstrap.
 */
export const deviceComputeProfilePromise: Promise<DeviceComputeProfile> = probeDeviceComputeProfile();

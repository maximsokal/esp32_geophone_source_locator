import { describe, expect, it } from 'vitest';

import {
  deriveDeviceComputeProfile,
  type DeviceComputeSignals,
} from '../../src/xr/deviceComputeProfile';

function signals(overrides: Partial<DeviceComputeSignals> = {}): DeviceComputeSignals {
  return {
    reportedLogicalProcessors: 8,
    workerApi: true,
    crossOriginIsolated: false,
    sharedArrayBufferApi: true,
    wasmApi: true,
    wasmSharedMemory: false,
    webGpuApi: true,
    webGpuAdapter: true,
    ...overrides,
  };
}

describe('device compute profile', () => {
  it('reserves XR/browser headroom instead of allocating every reported processor', () => {
    const profile = deriveDeviceComputeProfile(signals({ reportedLogicalProcessors: 8 }));
    expect(profile.reportedLogicalProcessors).toBe(8);
    expect(profile.reservedLogicalProcessors).toBe(2);
    expect(profile.liveWorkerBudget).toBe(2);
    expect(profile.postScanWorkerBudget).toBe(6);
    expect(profile.tier).toBe('high');
  });

  it('keeps live XR worker count conservative on a four-thread phone', () => {
    const profile = deriveDeviceComputeProfile(signals({ reportedLogicalProcessors: 4 }));
    expect(profile.reservedLogicalProcessors).toBe(1);
    expect(profile.liveWorkerBudget).toBe(2);
    expect(profile.postScanWorkerBudget).toBe(3);
    expect(profile.tier).toBe('balanced');
  });

  it('still permits one off-main worker on a single reported processor', () => {
    const profile = deriveDeviceComputeProfile(signals({ reportedLogicalProcessors: 1 }));
    expect(profile.reservedLogicalProcessors).toBe(0);
    expect(profile.liveWorkerBudget).toBe(1);
    expect(profile.postScanWorkerBudget).toBe(1);
    expect(profile.tier).toBe('limited');
  });

  it('does not invent CPU parallelism when the Worker API is unavailable', () => {
    const profile = deriveDeviceComputeProfile(signals({ workerApi: false }));
    expect(profile.liveWorkerBudget).toBe(0);
    expect(profile.postScanWorkerBudget).toBe(0);
    expect(profile.tier).toBe('limited');
  });

  it('requires cross-origin isolation before advertising shared-memory Wasm threads', () => {
    const notIsolated = deriveDeviceComputeProfile(signals({
      crossOriginIsolated: false,
      sharedArrayBufferApi: true,
      wasmSharedMemory: true,
    }));
    expect(notIsolated.sharedMemory).toBe(false);
    expect(notIsolated.wasmThreads).toBe(false);

    const isolated = deriveDeviceComputeProfile(signals({
      crossOriginIsolated: true,
      sharedArrayBufferApi: true,
      wasmSharedMemory: true,
    }));
    expect(isolated.sharedMemory).toBe(true);
    expect(isolated.wasmThreads).toBe(true);
  });

  it('does not call a WebGPU API hint a usable GPU without an adapter', () => {
    const profile = deriveDeviceComputeProfile(signals({ webGpuApi: true, webGpuAdapter: false }));
    expect(profile.webGpu).toBe(false);
  });

  it('caps privacy-noisy hardwareConcurrency values to a bounded scheduler input', () => {
    expect(deriveDeviceComputeProfile(signals({ reportedLogicalProcessors: 128 })).reportedLogicalProcessors).toBe(32);
    expect(deriveDeviceComputeProfile(signals({ reportedLogicalProcessors: 0 })).reportedLogicalProcessors).toBe(1);
  });
});

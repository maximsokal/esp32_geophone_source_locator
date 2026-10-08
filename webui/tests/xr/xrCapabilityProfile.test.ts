import { describe, expect, it } from 'vitest';

import { deriveDeviceComputeProfile } from '../../src/xr/deviceComputeProfile';
import {
  deriveNegotiatedXrDepthCapability,
  isTagReadbackPerformanceSafe,
  probeVideoInputInventory,
  summarizeComputeCapabilities,
  summarizeVideoInputDevices,
} from '../../src/xr/xrCapabilityProfile';

describe('XR capability profile', () => {
  it('treats only non-blocking PBO/fence readback as safe for high-resolution tag capture',()=>{
    expect(isTagReadbackPerformanceSafe('nonblocking')).toBe(true);
    expect(isTagReadbackPerformanceSafe('synchronous')).toBe(false);
    expect(isTagReadbackPerformanceSafe('unavailable')).toBe(false);
  });

  it('distinguishes negotiated raw CPU depth from physical sensor identity', () => {
    const depth = deriveNegotiatedXrDepthCapability({
      depthUsage: 'cpu-optimized',
      depthDataFormat: 'unsigned-short',
      depthType: 'raw',
      depthActive: true,
    });
    expect(depth.active).toBe(true);
    expect(depth.rawDepth).toBe(true);
    expect(depth.smoothDepth).toBe(false);
    expect(depth.cpuDepth).toBe(true);
    expect(depth.gpuDepth).toBe(false);
  });

  it('reports smooth GPU depth without pretending it is CPU-accessible', () => {
    const depth = deriveNegotiatedXrDepthCapability({
      depthUsage: 'gpu-optimized',
      depthDataFormat: 'float32',
      depthType: 'smooth',
      depthActive: true,
    });
    expect(depth.active).toBe(true);
    expect(depth.rawDepth).toBe(false);
    expect(depth.smoothDepth).toBe(true);
    expect(depth.cpuDepth).toBe(false);
    expect(depth.gpuDepth).toBe(true);
  });

  it('preserves negotiation metadata but marks paused/inactive depth unusable', () => {
    const depth = deriveNegotiatedXrDepthCapability({
      depthUsage: 'cpu-optimized',
      depthDataFormat: 'luminance-alpha',
      depthType: 'raw',
      depthActive: false,
    });
    expect(depth.active).toBe(false);
    expect(depth.usage).toBe('cpu-optimized');
    expect(depth.type).toBe('raw');
    expect(depth.rawDepth).toBe(false);
    expect(depth.cpuDepth).toBe(false);
  });

  it('never infers ToF or LiDAR hardware from camera labels', () => {
    const inventory = summarizeVideoInputDevices([
      { kind: 'videoinput', label: 'Back Camera' },
      { kind: 'videoinput', label: 'ToF Depth Camera' },
      { kind: 'videoinput', label: 'LiDAR / IR depth sensor' },
      { kind: 'audioinput', label: 'Microphone' },
    ]);
    expect(inventory.videoInputCount).toBe(3);
    expect(inventory.multipleVideoInputs).toBe(true);
    expect(inventory.labelsExposed).toBe(true);
    expect(inventory.hardwareDepthSensor).toBe('unknown');
    expect('tof' in inventory).toBe(false);
    expect('lidar' in inventory).toBe(false);
  });

  it('keeps failed device enumeration informational and non-fatal', async () => {
    const unavailable = await probeVideoInputInventory(undefined);
    expect(unavailable.enumerationAvailable).toBe(false);
    expect(unavailable.videoInputCount).toBeNull();
    expect(unavailable.hardwareDepthSensor).toBe('unknown');

    const denied = await probeVideoInputInventory({
      enumerateDevices: async () => { throw new Error('denied'); },
    });
    expect(denied.enumerationAvailable).toBe(true);
    expect(denied.enumerated).toBe(false);
    expect(denied.videoInputCount).toBeNull();
    expect(denied.hardwareDepthSensor).toBe('unknown');
  });

  it('reuses the existing compute profile instead of inventing a second scheduler probe', () => {
    const compute = deriveDeviceComputeProfile({
      reportedLogicalProcessors: 8,
      workerApi: true,
      crossOriginIsolated: true,
      sharedArrayBufferApi: true,
      wasmApi: true,
      wasmSharedMemory: true,
      webGpuApi: true,
      webGpuAdapter: true,
    });
    expect(summarizeComputeCapabilities(compute)).toEqual({
      workerApi: true,
      sharedMemory: true,
      wasmThreads: true,
      webGpu: true,
    });
  });
});

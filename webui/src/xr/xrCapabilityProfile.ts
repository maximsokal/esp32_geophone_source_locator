import type { DeviceComputeProfile } from './deviceComputeProfile';

/**
 * Browser JavaScript cannot reliably identify the physical depth hardware behind
 * ARCore/WebXR. Keep this explicitly unknown instead of guessing ToF/LiDAR/IR
 * from camera labels, model names, focal lengths or the presence of Depth API.
 */
export type HardwareDepthSensorStatus = 'unknown';

export type TagReadbackMode = 'nonblocking' | 'synchronous' | 'unavailable';

export function isTagReadbackPerformanceSafe(mode: TagReadbackMode): boolean {
  return mode === 'nonblocking';
}

export interface VideoInputDeviceLike {
  kind: string;
  label?: string;
}

export interface MediaDevicesCapabilityLike {
  enumerateDevices?: () => Promise<readonly VideoInputDeviceLike[]>;
}

export interface VideoInputInventory {
  enumerationAvailable: boolean;
  enumerated: boolean;
  videoInputCount: number | null;
  multipleVideoInputs: boolean;
  labelsExposed: boolean;
  /** Intentionally never inferred from MediaDeviceInfo.label. */
  hardwareDepthSensor: HardwareDepthSensorStatus;
}

export interface XrDepthSessionLike {
  depthUsage?: string | null;
  depthDataFormat?: string | null;
  depthType?: string | null;
  depthActive?: boolean | null;
}

export interface NegotiatedXrDepthCapability {
  active: boolean;
  usage: string | null;
  dataFormat: string | null;
  type: string | null;
  rawDepth: boolean;
  smoothDepth: boolean;
  cpuDepth: boolean;
  gpuDepth: boolean;
}

export interface ComputeCapabilitySummary {
  workerApi: boolean;
  sharedMemory: boolean;
  wasmThreads: boolean;
  webGpu: boolean;
}

const UNKNOWN_HARDWARE_DEPTH_SENSOR: HardwareDepthSensorStatus = 'unknown';

export function summarizeVideoInputDevices(
  devices: readonly VideoInputDeviceLike[],
): VideoInputInventory {
  let videoInputCount = 0;
  let labelsExposed = false;
  for (const device of devices) {
    if (device.kind !== 'videoinput') continue;
    videoInputCount += 1;
    if (typeof device.label === 'string' && device.label.trim().length > 0) labelsExposed = true;
  }
  return {
    enumerationAvailable: true,
    enumerated: true,
    videoInputCount,
    multipleVideoInputs: videoInputCount > 1,
    labelsExposed,
    hardwareDepthSensor: UNKNOWN_HARDWARE_DEPTH_SENSOR,
  };
}

/**
 * enumerateDevices() is informational only. It must never request permission and
 * its labels are never parsed as evidence of ToF/LiDAR/IR depth hardware.
 */
export async function probeVideoInputInventory(
  mediaDevices: MediaDevicesCapabilityLike | null | undefined,
): Promise<VideoInputInventory> {
  if (mediaDevices === null || mediaDevices === undefined || typeof mediaDevices.enumerateDevices !== 'function') {
    return {
      enumerationAvailable: false,
      enumerated: false,
      videoInputCount: null,
      multipleVideoInputs: false,
      labelsExposed: false,
      hardwareDepthSensor: UNKNOWN_HARDWARE_DEPTH_SENSOR,
    };
  }
  try {
    return summarizeVideoInputDevices(await mediaDevices.enumerateDevices());
  } catch {
    return {
      enumerationAvailable: true,
      enumerated: false,
      videoInputCount: null,
      multipleVideoInputs: false,
      labelsExposed: false,
      hardwareDepthSensor: UNKNOWN_HARDWARE_DEPTH_SENSOR,
    };
  }
}

/**
 * Report what the WebXR session actually negotiated. This is not a claim about
 * the phone's physical sensor: raw depth may be produced from motion/depth fusion
 * without a ToF camera, while smooth depth is UA-processed depth.
 */
export function deriveNegotiatedXrDepthCapability(
  session: XrDepthSessionLike,
): NegotiatedXrDepthCapability {
  const usage = typeof session.depthUsage === 'string' ? session.depthUsage : null;
  const dataFormat = typeof session.depthDataFormat === 'string' ? session.depthDataFormat : null;
  const type = typeof session.depthType === 'string' ? session.depthType : null;
  const active = session.depthActive !== false && (usage !== null || dataFormat !== null || type !== null);
  return {
    active,
    usage,
    dataFormat,
    type,
    rawDepth: active && type === 'raw',
    smoothDepth: active && type === 'smooth',
    cpuDepth: active && usage === 'cpu-optimized',
    gpuDepth: active && usage === 'gpu-optimized',
  };
}

export function summarizeComputeCapabilities(
  profile: DeviceComputeProfile,
): ComputeCapabilitySummary {
  return {
    workerApi: profile.workerApi,
    sharedMemory: profile.sharedMemory,
    wasmThreads: profile.wasmThreads,
    webGpu: profile.webGpu,
  };
}

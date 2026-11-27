import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';

/**
 * The latest pose-tagged RGB guidance frame, in a module with NO other imports, so that the depth-fusion worker can carry it without pulling the visual
 * evidence clients (and their workers) of xrGuidanceRegistry into its bundle. xrGuidanceRegistry re-exports these functions unchanged.
 */
let latestRgb: RgbDepthGuidanceFrame | null = null;
const POSE_TAGGED_RGB_MAX_AGE_MS = 650;

/** The stored frame with no age check (the registry's own bookkeeping). */
export function latestRgbGuidance(): RgbDepthGuidanceFrame | null { return latestRgb; }

export function setLatestRgbGuidance(frame: RgbDepthGuidanceFrame | null): void {
  latestRgb = frame;
}

export function getFreshRgbGuidance(nowMs: number, maximumAgeMs = 450): RgbDepthGuidanceFrame | null {
  if (latestRgb === null) return null;
  // Raw Camera Access frames carry their exact capture projection and pose. Such
  // a frame can safely remain available a little longer for historical 2D<->3D
  // reprojection. Live physical-depth fusion still applies rgbGuidanceMatchesView
  // before using it, so this grace period cannot attach stale RGB to a moved
  // camera pose. Legacy/unposed frames keep the caller's strict requested TTL.
  const poseTagged = latestRgb.projectionMatrix !== undefined && latestRgb.worldFromView !== undefined;
  const allowedAgeMs = poseTagged ? Math.max(maximumAgeMs, POSE_TAGGED_RGB_MAX_AGE_MS) : maximumAgeMs;
  if (nowMs - latestRgb.capturedAtMs > allowedAgeMs) return null;
  return latestRgb;
}

import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';
import { liveComputeScheduler } from './liveComputeScheduler';
import {
  setVisualLineDetectionDeferred,
  type VisualEvidenceFrame,
} from './visualLineEvidence';
import { PoseDiverseVisualEvidenceStore } from './visualEvidenceReservoir';
import { VisualLineEvidenceClient } from './visualLineEvidenceClient';

let latestRgb: RgbDepthGuidanceFrame | null = null;
const visualStore = new PoseDiverseVisualEvidenceStore(64);
// Live XR never runs Hough on the UI/XR thread. The worker is allocated lazily
// after the pre-scan compute capability promise resolves; if no worker slot is
// available, visual corroboration simply remains unavailable/fail-open.
const visualClient = new VisualLineEvidenceClient({
  allowMainThreadFallback: false,
  deferWorkerUntilAnalyze: true,
});
let visualDetectionError: string | null = null;
const POSE_TAGGED_RGB_MAX_AGE_MS = 650;

// WebXrRuntime still calls detectVisualLines() for compatibility, but production
// uses that call only as a cheap handoff marker. The real detector runs here in
// a module worker against the same pose-synchronized RGB frame.
setVisualLineDetectionDeferred(true);

export function setLatestRgbGuidance(frame: RgbDepthGuidanceFrame | null): void {
  latestRgb = frame;
}

export function getFreshRgbGuidance(nowMs: number, maximumAgeMs = 450): RgbDepthGuidanceFrame | null {
  if (latestRgb === null) return null;
  // Raw Camera Access frames carry their exact capture projection and pose. Such
  // a frame can safely remain available a little longer for historical 2D↔3D
  // reprojection. Live physical-depth fusion still applies rgbGuidanceMatchesView
  // before using it, so this grace period cannot attach stale RGB to a moved
  // camera pose. Legacy/unposed frames keep the caller's strict requested TTL.
  const poseTagged = latestRgb.projectionMatrix !== undefined && latestRgb.worldFromView !== undefined;
  const allowedAgeMs = poseTagged
    ? Math.max(maximumAgeMs, POSE_TAGGED_RGB_MAX_AGE_MS)
    : maximumAgeMs;
  if (nowMs - latestRgb.capturedAtMs > allowedAgeMs) return null;
  return latestRgb;
}

function usefulVisualFrame(frame: VisualEvidenceFrame): boolean {
  return frame.lines.length > 0 || frame.junctions.length > 0;
}

export function addVisualEvidenceFrame(frame: VisualEvidenceFrame): void {
  const guidance = latestRgb;
  const deferredMarker = frame.lines.length === 0 && frame.junctions.length === 0;
  if (
    deferredMarker && guidance !== null &&
    Math.abs(guidance.capturedAtMs - frame.capturedAtMs) <= 20 &&
    !visualClient.busy
  ) {
    // The detector works on the RGB pixels, so reprojection must use the exact
    // pose/projection that captured those pixels. Falling back to the marker's
    // matrices preserves synthetic/offline callers that do not carry capture
    // metadata, but production Raw Camera frames always provide it.
    const projectionMatrix = guidance.projectionMatrix ?? frame.projectionMatrix;
    const worldFromView = guidance.worldFromView ?? frame.worldFromView;
    void visualClient.analyze(
      guidance,
      frame.keyframeId,
      projectionMatrix,
      worldFromView,
    ).then((resolved) => {
      visualDetectionError = null;
      // Empty Hough results do not consume the bounded reservoir. The displayed
      // count therefore represents frames that can actually validate geometry.
      if (usefulVisualFrame(resolved)) visualStore.add(resolved);
    }).catch((error: unknown) => {
      visualDetectionError = error instanceof Error ? error.message : String(error);
    });
    return;
  }

  // Isolated/offline callers can still supply already-detected evidence.
  if (!deferredMarker && usefulVisualFrame(frame)) visualStore.add(frame);
}

export function getVisualEvidenceFrames(): readonly VisualEvidenceFrame[] {
  return visualStore.snapshot();
}

export function getVisualEvidenceFrameCount(): number { return visualStore.size; }
export function isVisualEvidenceWorkerBacked(): boolean { return visualClient.workerBacked; }
export function getVisualEvidenceDetectionError(): string | null { return visualDetectionError; }

/**
 * Reset all pose-synchronised guidance for a new immersive scan epoch.
 *
 * WebXrRuntime calls this from resetScanState(), making this the existing XR scan
 * lifecycle boundary shared by visual evidence. Reset the scheduler's session
 * hysteresis counters here as well. LiveComputeScheduler.beginScan() deliberately
 * preserves the last pressure cap, so a hot phone cannot recover W2 merely by
 * ending and immediately restarting WebXR; sustained low-pressure windows must
 * earn the extra slot back.
 */
export function resetXrGuidanceRegistry(): void {
  latestRgb = null;
  visualStore.reset();
  visualClient.reset();
  liveComputeScheduler.beginScan();
  visualDetectionError = null;
}

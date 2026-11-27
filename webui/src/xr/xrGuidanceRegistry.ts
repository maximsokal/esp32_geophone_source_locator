import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';
import { getFreshRgbGuidance, latestRgbGuidance, setLatestRgbGuidance } from './rgbGuidanceStore';
import { liveComputeScheduler } from './liveComputeScheduler';
import {
  setVisualLineDetectionDeferred,
  type VisualEvidenceFrame,
} from './visualLineEvidence';
import { PoseDiverseVisualEvidenceStore } from './visualEvidenceReservoir';
import { VisualLineEvidenceClient } from './visualLineEvidenceClient';

const visualStore = new PoseDiverseVisualEvidenceStore(64);
// Live XR never runs Hough on the UI/XR thread. The worker is allocated lazily
// after the pre-scan compute capability promise resolves; if no worker slot is
// available, visual corroboration simply remains unavailable/fail-open.
function createVisualClient(): VisualLineEvidenceClient {
  return new VisualLineEvidenceClient({
  allowMainThreadFallback: false,
  deferWorkerUntilAnalyze: true,
  });
}
let visualClient = createVisualClient();
let visualClientGeneration = 1;
let visualDetectionError: string | null = null;

// WebXrRuntime still calls detectVisualLines() for compatibility, but production
// uses that call only as a cheap handoff marker. The real detector runs here in
// a module worker against the same pose-synchronized RGB frame.
setVisualLineDetectionDeferred(true);

export { getFreshRgbGuidance, setLatestRgbGuidance };

function usefulVisualFrame(frame: VisualEvidenceFrame): boolean {
  return frame.lines.length > 0 || frame.junctions.length > 0;
}

export function addVisualEvidenceFrame(frame: VisualEvidenceFrame): void {
  const guidance = latestRgbGuidance();
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
    const client = visualClient;
    const generation = visualClientGeneration;
    void client.analyze(
      guidance,
      frame.keyframeId,
      projectionMatrix,
      worldFromView,
    ).then((resolved) => {
      if (generation !== visualClientGeneration) return;
      visualDetectionError = null;
      // Empty Hough results do not consume the bounded reservoir. The displayed
      // count therefore represents frames that can actually validate geometry.
      if (usefulVisualFrame(resolved)) visualStore.add(resolved);
    }).catch((error: unknown) => {
      if (generation !== visualClientGeneration) return;
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
export function getVisualEvidenceClientGeneration(): number { return visualClientGeneration; }

/** Release the role-owned live worker at Finish while keeping the registry reusable. */
export function releaseXrGuidanceRegistryWorkers(): void {
  setLatestRgbGuidance(null);
  visualStore.reset();
  visualClient.dispose();
  visualClient = createVisualClient();
  visualClientGeneration += 1;
  visualDetectionError = null;
}

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
  setLatestRgbGuidance(null);
  visualStore.reset();
  releaseXrGuidanceRegistryWorkers();
  liveComputeScheduler.beginScan();
  visualDetectionError = null;
}

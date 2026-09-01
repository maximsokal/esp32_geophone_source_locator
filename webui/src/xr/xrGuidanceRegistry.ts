import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';
import {
  setVisualLineDetectionDeferred,
  type VisualEvidenceFrame,
} from './visualLineEvidence';
import { PoseDiverseVisualEvidenceStore } from './visualEvidenceReservoir';
import { VisualLineEvidenceClient } from './visualLineEvidenceClient';

let latestRgb: RgbDepthGuidanceFrame | null = null;
const visualStore = new PoseDiverseVisualEvidenceStore(64);
const visualClient = new VisualLineEvidenceClient();
let visualDetectionError: string | null = null;

// WebXrRuntime still calls detectVisualLines() for compatibility, but production
// uses that call only as a cheap handoff marker. The real detector runs here in
// a module worker against the same pose-synchronized RGB frame.
setVisualLineDetectionDeferred(true);

export function setLatestRgbGuidance(frame: RgbDepthGuidanceFrame | null): void {
  latestRgb = frame;
}

export function getFreshRgbGuidance(nowMs: number, maximumAgeMs = 450): RgbDepthGuidanceFrame | null {
  if (latestRgb === null || nowMs - latestRgb.capturedAtMs > maximumAgeMs) return null;
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

export function resetXrGuidanceRegistry(): void {
  latestRgb = null;
  visualStore.reset();
  visualClient.reset();
  visualDetectionError = null;
}
import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';
import { VisualLineEvidenceStore, type VisualEvidenceFrame } from './visualLineEvidence';

let latestRgb: RgbDepthGuidanceFrame | null = null;
const visualStore = new VisualLineEvidenceStore(18);

export function setLatestRgbGuidance(frame: RgbDepthGuidanceFrame | null): void {
  latestRgb = frame;
}

export function getFreshRgbGuidance(nowMs: number, maximumAgeMs = 450): RgbDepthGuidanceFrame | null {
  if (latestRgb === null || nowMs - latestRgb.capturedAtMs > maximumAgeMs) return null;
  return latestRgb;
}

export function addVisualEvidenceFrame(frame: VisualEvidenceFrame): void {
  visualStore.add(frame);
}

export function getVisualEvidenceFrames(): readonly VisualEvidenceFrame[] {
  return visualStore.snapshot();
}

export function getVisualEvidenceFrameCount(): number { return visualStore.size; }

export function resetXrGuidanceRegistry(): void {
  latestRgb = null;
  visualStore.reset();
}

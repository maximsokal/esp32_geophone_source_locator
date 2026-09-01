import type { ReprojectionCertificate } from './reprojectionCertificate';

export interface StructuralVisualCorroboration {
  confidence: number;
  /** Physical multi-view observations only. RGB matches never increase this. */
  observations: number;
  visuallyConfirmed: boolean;
  visualDisagreement: boolean;
  eligibleVisualObservations: number;
  matchedVisualObservations: number;
}

/**
 * Raw-camera line/junction detection is intentionally asymmetric evidence.
 * A positive multi-view reprojection match can corroborate robust metric 3D
 * geometry, but a missed Hough line is not strong enough to erase that geometry:
 * blur, texture, clipping and lighting can all suppress the image-space line.
 * Physical ARCore/free-space checks remain the hard veto path.
 */
export function applyStructuralVisualCorroboration(
  baseConfidence: number,
  baseObservations: number,
  visual: ReprojectionCertificate | null,
): StructuralVisualCorroboration {
  const confidence = Math.max(0, Math.min(1, baseConfidence));
  const observations = Math.max(0, Math.floor(baseObservations));
  if (visual === null || !visual.conclusive) {
    return {
      confidence,
      observations,
      visuallyConfirmed: false,
      visualDisagreement: false,
      eligibleVisualObservations: visual?.eligibleObservations ?? 0,
      matchedVisualObservations: visual?.observations ?? 0,
    };
  }

  if (!visual.accepted) {
    return {
      confidence,
      observations,
      visuallyConfirmed: false,
      visualDisagreement: true,
      eligibleVisualObservations: visual.eligibleObservations,
      matchedVisualObservations: visual.observations,
    };
  }

  // Positive RGB evidence may add a small confidence bonus, but cannot dominate
  // the physical 3D certificate. Visual matches are reported separately and do
  // not count as independent metric ARCore observations.
  const bonus = (1 - confidence) * Math.max(0, Math.min(1, visual.confidence)) * 0.08;
  return {
    confidence: Math.min(0.97, confidence + bonus),
    observations,
    visuallyConfirmed: true,
    visualDisagreement: false,
    eligibleVisualObservations: visual.eligibleObservations,
    matchedVisualObservations: visual.observations,
  };
}

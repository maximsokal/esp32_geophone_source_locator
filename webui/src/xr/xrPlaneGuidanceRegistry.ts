import {
  captureStructuralPlanePriors,
  type StructuralPlanePrior,
  type XrPlaneFrameLike,
} from './webxrPlaneGuidance';

let latestPriors: readonly StructuralPlanePrior[] = [];

function clonePrior(prior: StructuralPlanePrior): StructuralPlanePrior {
  return {
    ...prior,
    center: [...prior.center] as readonly [number, number, number],
    normal: [...prior.normal] as readonly [number, number, number],
    tangentU: [...prior.tangentU] as readonly [number, number, number],
    tangentV: [...prior.tangentV] as readonly [number, number, number],
    polygonUv: prior.polygonUv.map((point) => [point[0], point[1]] as const),
  };
}

/** Store only owned numeric snapshots; no frame-scoped XRPlane/XRSpace escapes here. */
export function setLatestStructuralPlanePriors(priors: readonly StructuralPlanePrior[]): void {
  latestPriors = priors.map(clonePrior);
}

export function getLatestStructuralPlanePriors(): readonly StructuralPlanePrior[] {
  return latestPriors.map(clonePrior);
}

export function getLatestStructuralPlanePriorCount(): number { return latestPriors.length; }

/**
 * Snapshot native planes while the XRFrame is active. Unsupported Plane
 * Detection is intentionally equivalent to an empty advisory set.
 */
export function captureLatestStructuralPlanePriors(
  frame: XrPlaneFrameLike,
  referenceSpace: object,
): number {
  const priors = captureStructuralPlanePriors(frame, referenceSpace);
  setLatestStructuralPlanePriors(priors);
  return priors.length;
}

export function resetXrPlaneGuidanceRegistry(): void { latestPriors = []; }

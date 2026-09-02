import type { StructuralPlanePrior } from './webxrPlaneGuidance';

let latestPriors: readonly StructuralPlanePrior[] = [];

function clonePrior(prior: StructuralPlanePrior): StructuralPlanePrior {
  return {
    ...prior,
    center: [...prior.center] as readonly [number, number, number],
    normal: [...prior.normal] as readonly [number, number, number],
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

export function resetXrPlaneGuidanceRegistry(): void { latestPriors = []; }

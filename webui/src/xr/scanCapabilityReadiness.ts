import type { StructuralGuideViewpoint } from './structuralGuide';
import type { SurfaceComponentAnalysis, SurfaceComponentPoint } from './surfaceComponents';

export interface ScanCapabilityInput {
  stablePointCount: number;
  keyframeCount: number;
  visibleCoverageCells: number;
  readyCoverageCells: number;
  surfaceAnalysis?: SurfaceComponentAnalysis | null;
  viewpoints?: readonly StructuralGuideViewpoint[];
}

export interface CapabilityProgress {
  progress: number;
  ready: boolean;
  limitingMetric: 'points' | 'views' | 'extent' | 'surface-density' | 'unknown';
  growthNeededPercent: number | null;
}

export interface ScanCapabilityReadiness {
  structural: CapabilityProgress;
  mesh: CapabilityProgress | null;
  viewCoverage: number;
  viewSectors: number;
  totalViewSectors: number;
  cameraBaselineMeters: number;
  objectMajorExtentMeters: number | null;
  objectMinorExtentMeters: number | null;
  objectTooSmall: boolean;
}

const STRUCTURAL_TARGET_POINTS = 180;
const STRUCTURAL_TARGET_KEYFRAMES = 8;
const STRUCTURAL_COMPONENT_POINTS = 18;
const STRUCTURAL_MAJOR_EXTENT_METERS = 0.12;
const STRUCTURAL_MINOR_EXTENT_METERS = 0.045;
const VIEW_SECTORS = 16;
const MIN_VIEW_RADIUS_METERS = 0.10;
const TARGET_CAMERA_BASELINE_METERS = 0.28;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function ratio(value: number, target: number): number { return target <= 0 ? 1 : clamp01(value / target); }
function distance(a: readonly [number, number, number], b: readonly [number, number, number]): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
function relativeGrowthNeeded(progress: number): number | null {
  if (!Number.isFinite(progress) || progress <= 0 || progress >= 1) return progress >= 1 ? 0 : null;
  return Math.max(1, Math.ceil((1 / progress - 1) * 100));
}

function cameraBaseline(viewpoints: readonly StructuralGuideViewpoint[]): number {
  let maximum = 0;
  for (let first = 0; first < viewpoints.length; first += 1) {
    for (let second = first + 1; second < viewpoints.length; second += 1) {
      maximum = Math.max(maximum, distance(viewpoints[first].position, viewpoints[second].position));
    }
  }
  return maximum;
}

function azimuthCoverage(
  viewpoints: readonly StructuralGuideViewpoint[],
  center: SurfaceComponentPoint | null,
): { sectors: number; fraction: number } {
  if (center === null || viewpoints.length === 0) return { sectors: 0, fraction: 0 };
  const sectors = new Set<number>();
  for (const viewpoint of viewpoints) {
    // Project coordinates use z as vertical; x/y span the horizontal plane.
    const dx = viewpoint.position[0] - center[0];
    const dy = viewpoint.position[1] - center[1];
    if (Math.hypot(dx, dy) < MIN_VIEW_RADIUS_METERS) continue;
    const normalized = (Math.atan2(dy, dx) + Math.PI * 2) % (Math.PI * 2);
    sectors.add(Math.min(VIEW_SECTORS - 1, Math.floor(normalized / (Math.PI * 2) * VIEW_SECTORS)));
  }
  return { sectors: sectors.size, fraction: sectors.size / VIEW_SECTORS };
}

function structuralScaleProgress(surface: SurfaceComponentAnalysis | null | undefined): {
  progress: number;
  limitingMetric: CapabilityProgress['limitingMetric'];
} | null {
  const component = surface?.largestComponent ?? null;
  if (component === null) return null;
  const pointProgress = ratio(component.pointCount, STRUCTURAL_COMPONENT_POINTS);
  const majorProgress = ratio(component.majorExtentMeters, STRUCTURAL_MAJOR_EXTENT_METERS);
  const minorProgress = ratio(component.minorExtentMeters, STRUCTURAL_MINOR_EXTENT_METERS);
  const progress = Math.min(pointProgress, majorProgress, minorProgress);
  if (progress === pointProgress) return { progress, limitingMetric: 'surface-density' };
  return { progress, limitingMetric: 'extent' };
}

/**
 * Capability readiness is a conjunction, not a score average. One excellent
 * signal cannot hide a missing prerequisite: many points cannot compensate for
 * a physically tiny patch, and a large patch cannot compensate for missing
 * independent views. Percentages therefore use the weakest required branch.
 */
export function computeScanCapabilityReadiness(input: ScanCapabilityInput): ScanCapabilityReadiness {
  const pointsProgress = ratio(input.stablePointCount, STRUCTURAL_TARGET_POINTS);
  const keyframeProgress = ratio(input.keyframeCount, STRUCTURAL_TARGET_KEYFRAMES);
  const scale = structuralScaleProgress(input.surfaceAnalysis);
  const structuralTerms = [pointsProgress, keyframeProgress];
  if (scale !== null) structuralTerms.push(scale.progress);
  const structuralProgress = Math.min(...structuralTerms);
  let structuralLimit: CapabilityProgress['limitingMetric'] = pointsProgress <= keyframeProgress ? 'points' : 'views';
  if (scale !== null && scale.progress <= structuralProgress + 1e-9) structuralLimit = scale.limitingMetric;

  const bestMesh = input.surfaceAnalysis?.bestMeshComponent ?? null;
  const mesh = bestMesh === null ? null : {
    progress: clamp01(bestMesh.meshReadiness),
    ready: bestMesh.meshEligible,
    limitingMetric: bestMesh.pointCount < 24 ? 'points' as const
      : bestMesh.majorExtentMeters < 0.14 || bestMesh.minorExtentMeters < 0.075 ? 'extent' as const
        : 'surface-density' as const,
    growthNeededPercent: relativeGrowthNeeded(bestMesh.meshReadiness),
  };

  const viewpoints = input.viewpoints ?? [];
  const baseline = cameraBaseline(viewpoints);
  const largest = input.surfaceAnalysis?.largestComponent ?? null;
  const azimuth = azimuthCoverage(viewpoints, largest?.center ?? null);
  // Before a stable object centre exists, report conservative translation/view
  // progress rather than pretending that a raw phone rotation equals 360° object
  // coverage. Once centred surface geometry exists, the percentage is actual
  // occupied azimuth sectors around that measured component.
  const viewCoverage = largest === null
    ? Math.min(ratio(viewpoints.length, STRUCTURAL_TARGET_KEYFRAMES), ratio(baseline, TARGET_CAMERA_BASELINE_METERS))
    : azimuth.fraction;

  const objectMajor = largest?.majorExtentMeters ?? null;
  const objectMinor = largest?.minorExtentMeters ?? null;
  return {
    structural: {
      progress: structuralProgress,
      ready: structuralProgress >= 1 - 1e-9,
      limitingMetric: structuralLimit,
      growthNeededPercent: relativeGrowthNeeded(structuralProgress),
    },
    mesh,
    viewCoverage: clamp01(viewCoverage),
    viewSectors: azimuth.sectors,
    totalViewSectors: VIEW_SECTORS,
    cameraBaselineMeters: baseline,
    objectMajorExtentMeters: objectMajor,
    objectMinorExtentMeters: objectMinor,
    objectTooSmall: scale !== null && scale.limitingMetric === 'extent' && scale.progress < 1,
  };
}

export class ScanViewpointHistory {
  private readonly values: StructuralGuideViewpoint[] = [];
  private lastKeyframeId = -1;

  constructor(private readonly maximum = 64) {}

  reset(): void {
    this.values.length = 0;
    this.lastKeyframeId = -1;
  }

  record(viewpoint: StructuralGuideViewpoint | null | undefined): boolean {
    if (viewpoint === null || viewpoint === undefined || !Number.isFinite(viewpoint.keyframeId)) return false;
    if (viewpoint.keyframeId === this.lastKeyframeId) return false;
    this.lastKeyframeId = viewpoint.keyframeId;
    this.values.push({
      ...viewpoint,
      position: [...viewpoint.position] as [number, number, number],
      forward: viewpoint.forward === undefined ? undefined : [...viewpoint.forward] as [number, number, number],
    });
    if (this.values.length > this.maximum) this.values.splice(0, this.values.length - this.maximum);
    return true;
  }

  snapshot(): readonly StructuralGuideViewpoint[] {
    return this.values.map((viewpoint) => ({
      ...viewpoint,
      position: [...viewpoint.position] as [number, number, number],
      forward: viewpoint.forward === undefined ? undefined : [...viewpoint.forward] as [number, number, number],
    }));
  }
}

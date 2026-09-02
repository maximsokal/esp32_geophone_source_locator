import type { StructuralGuideViewpoint } from './structuralGuide';
import {
  MESH_MIN_POINTS,
  type SurfaceComponentAnalysis,
  type SurfaceComponentPoint,
} from './surfaceComponents';

type SurfaceComponent = SurfaceComponentAnalysis['components'][number];
type ReadinessSurface = Pick<SurfaceComponentAnalysis, 'components' | 'bestMeshComponent'>;

/**
 * Small worker -> UI contract. The full component analysis can contain hundreds
 * of disconnected islands and large point masks; none of that is needed by the
 * readiness bar. Keep only the component that best supports structural geometry
 * and the independently best component from the actual mesh input policy.
 */
export interface SurfaceComponentReadinessSummary {
  structuralComponent: SurfaceComponent | null;
  bestMeshComponent: SurfaceComponent | null;
}

type LegacySurfaceReadinessInput = ReadinessSurface;
export type SurfaceComponentReadinessInput = SurfaceComponentReadinessSummary | LegacySurfaceReadinessInput;

export interface ScanCapabilityInput {
  stablePointCount: number;
  keyframeCount: number;
  visibleCoverageCells: number;
  readyCoverageCells: number;
  surfaceAnalysis?: SurfaceComponentReadinessInput | null;
  viewpoints?: readonly StructuralGuideViewpoint[];
}

export interface CapabilityProgress {
  progress: number;
  ready: boolean;
  limitingMetric: 'points' | 'views' | 'extent' | 'surface-density' | 'unknown';
  growthNeededPercent: number | null;
}

export interface StructuralSurfaceReadiness {
  progress: number;
  ready: boolean;
  limitingMetric: CapabilityProgress['limitingMetric'];
  component: SurfaceComponent | null;
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
  /** True only when physical extent is the branch currently limiting STRUCT. */
  objectTooSmall: boolean;
}

const STRUCTURAL_TARGET_POINTS = 180;
const STRUCTURAL_TARGET_KEYFRAMES = 8;
const STRUCTURAL_TARGET_BASELINE_METERS = 0.12;
const STRUCTURAL_COMPONENT_POINTS = 18;
const STRUCTURAL_MAJOR_EXTENT_METERS = 0.12;
const STRUCTURAL_MINOR_EXTENT_METERS = 0.045;
const MESH_MIN_OCCUPIED_CELLS = 7;
const MESH_MIN_OCCUPANCY_RATIO = 0.30;
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
    const dx = viewpoint.position[0] - center[0];
    const dy = viewpoint.position[1] - center[1];
    if (Math.hypot(dx, dy) < MIN_VIEW_RADIUS_METERS) continue;
    const normalized = (Math.atan2(dy, dx) + Math.PI * 2) % (Math.PI * 2);
    sectors.add(Math.min(VIEW_SECTORS - 1, Math.floor(normalized / (Math.PI * 2) * VIEW_SECTORS)));
  }
  return { sectors: sectors.size, fraction: sectors.size / VIEW_SECTORS };
}

interface StructuralComponentCandidate {
  component: SurfaceComponent;
  progress: number;
  limitingMetric: CapabilityProgress['limitingMetric'];
}

function componentStructuralProgress(component: SurfaceComponent): Omit<StructuralComponentCandidate, 'component'> {
  const pointProgress = ratio(component.pointCount, STRUCTURAL_COMPONENT_POINTS);
  const majorProgress = ratio(component.majorExtentMeters, STRUCTURAL_MAJOR_EXTENT_METERS);
  const minorProgress = ratio(component.minorExtentMeters, STRUCTURAL_MINOR_EXTENT_METERS);
  const progress = Math.min(pointProgress, majorProgress, minorProgress);
  if (progress === pointProgress) return { progress, limitingMetric: 'surface-density' };
  return { progress, limitingMetric: 'extent' };
}

function bestStructuralComponent(components: readonly SurfaceComponent[]): StructuralComponentCandidate | null {
  if (components.length === 0) return null;
  let best: StructuralComponentCandidate | null = null;
  for (const component of components) {
    const candidate = componentStructuralProgress(component);
    const area = component.majorExtentMeters * component.minorExtentMeters;
    const bestArea = best === null ? -1 : best.component.majorExtentMeters * best.component.minorExtentMeters;
    if (
      best === null ||
      candidate.progress > best.progress ||
      (candidate.progress === best.progress && area > bestArea) ||
      (candidate.progress === best.progress && area === bestArea && component.pointCount > best.component.pointCount)
    ) {
      best = { component, ...candidate };
    }
  }
  return best;
}

function isCompactSurfaceReadiness(
  surface: SurfaceComponentReadinessInput,
): surface is SurfaceComponentReadinessSummary {
  return 'structuralComponent' in surface;
}

/**
 * Build the bounded worker -> UI summary without returning masks or island lists.
 *
 * Structural and mesh readiness may intentionally use different physical views of
 * the same scan. Structural geometry uses clustered normal families so sharp
 * manifolds survive robust fitting; mesh readiness must use the locally certified
 * surfels that the TSDF path will actually receive, without structural-family
 * clustering. Supplying `meshSurface` keeps Mxx honest without changing the
 * historical one-argument API used by tests/legacy callers.
 */
export function summarizeSurfaceComponentReadiness(
  structuralSurface: ReadinessSurface,
  meshSurface: ReadinessSurface = structuralSurface,
): SurfaceComponentReadinessSummary {
  return {
    structuralComponent: bestStructuralComponent(structuralSurface.components)?.component ?? null,
    bestMeshComponent: meshSurface.bestMeshComponent,
  };
}

/**
 * Cheap policy boundary shared by the worker and the UI. It contains only the
 * physical connected-surface prerequisites for structural geometry; camera
 * baseline/keyframe prerequisites remain part of computeScanCapabilityReadiness.
 */
export function computeSurfaceStructuralReadiness(
  surface: SurfaceComponentReadinessInput | null | undefined,
): StructuralSurfaceReadiness {
  const component = surface === null || surface === undefined
    ? null
    : isCompactSurfaceReadiness(surface)
      ? surface.structuralComponent
      : bestStructuralComponent(surface.components)?.component ?? null;
  if (component === null) {
    return {
      progress: 0,
      ready: false,
      limitingMetric: 'surface-density',
      component: null,
    };
  }
  const candidate = componentStructuralProgress(component);
  return {
    progress: candidate.progress,
    ready: candidate.progress >= 1 - 1e-9,
    limitingMetric: candidate.limitingMetric,
    component,
  };
}

/**
 * SurfaceComponents computes meshReadiness from five conjunctive physical gates:
 * points, occupied cells, scale-aware major extent, scale-aware minor extent and
 * occupancy ratio. The two extent targets depend on voxel size. Do not duplicate
 * old fixed centimetre thresholds here: once the shared point/density gates are
 * satisfied, any remaining readiness deficit necessarily comes from metric scale.
 */
function meshLimitingMetric(component: SurfaceComponent): CapabilityProgress['limitingMetric'] {
  if (component.pointCount < MESH_MIN_POINTS) return 'points';
  if (
    component.occupiedCellCount < MESH_MIN_OCCUPIED_CELLS ||
    component.occupancyRatio < MESH_MIN_OCCUPANCY_RATIO
  ) return 'surface-density';
  if (!component.meshEligible || component.meshReadiness < 1 - 1e-9) return 'extent';
  return 'surface-density';
}

function bestMeshComponent(
  surface: SurfaceComponentReadinessInput | null | undefined,
): SurfaceComponent | null {
  if (surface === null || surface === undefined) return null;
  return surface.bestMeshComponent;
}

/**
 * Capability readiness is a conjunction, not a score average. One excellent
 * signal cannot hide a missing prerequisite: many points cannot compensate for
 * a physically tiny patch, keyframe count cannot compensate for a stationary
 * camera, and a large patch cannot compensate for missing independent views.
 */
export function computeScanCapabilityReadiness(input: ScanCapabilityInput): ScanCapabilityReadiness {
  const pointsProgress = ratio(input.stablePointCount, STRUCTURAL_TARGET_POINTS);
  const keyframeProgress = ratio(input.keyframeCount, STRUCTURAL_TARGET_KEYFRAMES);
  const viewpoints = input.viewpoints ?? [];
  const baseline = cameraBaseline(viewpoints);
  const translationProgress = ratio(baseline, STRUCTURAL_TARGET_BASELINE_METERS);
  const scale = computeSurfaceStructuralReadiness(input.surfaceAnalysis);
  const structuralTerms = [pointsProgress, keyframeProgress, translationProgress];
  if (scale.component !== null) structuralTerms.push(scale.progress);
  const structuralProgress = Math.min(...structuralTerms);
  let structuralLimit: CapabilityProgress['limitingMetric'] = 'views';
  if (pointsProgress <= structuralProgress + 1e-9) structuralLimit = 'points';
  else if (keyframeProgress <= structuralProgress + 1e-9 || translationProgress <= structuralProgress + 1e-9) structuralLimit = 'views';
  if (scale.component !== null && scale.progress <= structuralProgress + 1e-9) structuralLimit = scale.limitingMetric;

  const bestMesh = bestMeshComponent(input.surfaceAnalysis);
  const mesh = bestMesh === null ? null : {
    progress: clamp01(bestMesh.meshReadiness),
    ready: bestMesh.meshEligible,
    limitingMetric: meshLimitingMetric(bestMesh),
    growthNeededPercent: relativeGrowthNeeded(bestMesh.meshReadiness),
  };

  const structuralComponent = scale.component;
  const azimuth = azimuthCoverage(viewpoints, structuralComponent?.center ?? null);
  const viewCoverage = structuralComponent === null
    ? Math.min(ratio(viewpoints.length, STRUCTURAL_TARGET_KEYFRAMES), ratio(baseline, TARGET_CAMERA_BASELINE_METERS))
    : azimuth.fraction;

  const objectMajor = structuralComponent?.majorExtentMeters ?? null;
  const objectMinor = structuralComponent?.minorExtentMeters ?? null;
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
    // The overlay labels structural.growthNeededPercent as AREA only when extent
    // is the actual weakest prerequisite. If points or views are lower, showing
    // AREA +400% would be numerically wrong and misleading to the operator.
    objectTooSmall: structuralLimit === 'extent' && structuralProgress < 1,
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

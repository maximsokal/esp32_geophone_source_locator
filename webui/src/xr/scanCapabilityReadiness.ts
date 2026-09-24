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
  /** MAX-254: can the measured faces carry an edge / a corner at all? */
  structuralFaces?: StructuralFaceCapability;
}

/**
 * MAX-254: structural sufficiency is about FACES, not one surface. An edge
 * needs two adjacent, non-parallel faces that are each sampled densely enough
 * for the edge detector; a corner needs three with a well-conditioned
 * intersection. Per-face density is points per estimated planar area.
 */
export interface StructuralFaceCapability {
  voxelSizeMeters: number;
  /** Per-face progress toward the structural face target (0..1), by component id. */
  faceProgress: ReadonlyArray<{ id: number; progress: number; limitingMetric: CapabilityProgress['limitingMetric'] }>;
  edgeComponents: readonly [SurfaceComponent, SurfaceComponent] | null;
  edgeProgress: number;
  cornerComponents: readonly [SurfaceComponent, SurfaceComponent, SurfaceComponent] | null;
  cornerProgress: number;
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
  /** 'faces': not enough distinct adjacent faces for an edge yet (scan another side). */
  limitingMetric: 'points' | 'views' | 'extent' | 'surface-density' | 'faces' | 'unknown';
  growthNeededPercent: number | null;
}

export interface StructuralSurfaceReadiness {
  /** Progress toward EDGE capability (the structural worker's gate). */
  progress: number;
  ready: boolean;
  limitingMetric: CapabilityProgress['limitingMetric'];
  component: SurfaceComponent | null;
  capability: { surface: boolean; edge: boolean; corner: boolean };
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
/*
 * Calibrated through the production path (prepareStructuralGuideReadiness ->
 * structural surface -> this area estimator -> edge/corner detector on the
 * same surface; 4 cm voxels, jittered sampling + 4 mm noise, 12 placements):
 * true 5.5 cm spacing -> measured face density p10 274 /m^2, corner 12/12;
 * true 6 cm -> median 273 /m^2, corner 8/12; 6.5 cm -> faces fragment, 0/12.
 * Target: measured density of one point per (1.5 voxel)^2 = 278 /m^2.
 */
const STRUCTURAL_MAX_SPACING_VOXELS = 1.5;
/** Detector pair gate (structuralGuide MIN_PLANE_ANGLE_RAD). */
const STRUCTURAL_EDGE_MIN_ANGLE_RAD = (34 * Math.PI) / 180;
/** Detector three-plane conditioning (structuralGuide solveThreePlanes). */
const STRUCTURAL_CORNER_MIN_DETERMINANT = 0.12;
/** Faces meeting at an edge have points this close (> max sampling spacing + noise). */
const STRUCTURAL_FACE_ADJACENCY_METERS = 0.08;
const MESH_MIN_OCCUPIED_CELLS = 7;
const MESH_MIN_OCCUPANCY_RATIO = 0.30;
const VIEW_SECTORS = 16;
const MIN_VIEW_RADIUS_METERS = 0.10;
const TARGET_CAMERA_BASELINE_METERS = 0.28;

function clamp(value: number, minimum: number, maximum: number): number { return Math.max(minimum, Math.min(maximum, value)); }
function clamp01(value: number): number { return clamp(value, 0, 1); }
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

export function structuralFaceDensityTarget(voxelSizeMeters: number): number {
  const spacing = STRUCTURAL_MAX_SPACING_VOXELS * clamp(Math.abs(voxelSizeMeters) || 0.04, 0.02, 0.10);
  return 1 / (spacing * spacing);
}

function componentStructuralProgress(
  component: SurfaceComponent,
  voxelSizeMeters?: number,
): Omit<StructuralComponentCandidate, 'component'> {
  const pointProgress = ratio(component.pointCount, STRUCTURAL_COMPONENT_POINTS);
  const majorProgress = ratio(component.majorExtentMeters, STRUCTURAL_MAJOR_EXTENT_METERS);
  const minorProgress = ratio(component.minorExtentMeters, STRUCTURAL_MINOR_EXTENT_METERS);
  const area = component.estimatedAreaSquareMeters;
  const densityProgress = voxelSizeMeters !== undefined && area !== undefined && area > 0
    ? ratio(component.pointCount / area, structuralFaceDensityTarget(voxelSizeMeters))
    : 1;
  const progress = Math.min(pointProgress, majorProgress, minorProgress, densityProgress);
  if (progress === pointProgress || progress === densityProgress) return { progress, limitingMetric: 'surface-density' };
  return { progress, limitingMetric: 'extent' };
}

function bestStructuralComponent(components: readonly SurfaceComponent[], voxelSizeMeters?: number): StructuralComponentCandidate | null {
  if (components.length === 0) return null;
  let best: StructuralComponentCandidate | null = null;
  for (const component of components) {
    const candidate = componentStructuralProgress(component, voxelSizeMeters);
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

function normalAngle(a: SurfaceComponent, b: SurfaceComponent): number {
  if (a.normal === undefined || b.normal === undefined) return 0;
  const d = Math.abs(a.normal[0] * b.normal[0] + a.normal[1] * b.normal[1] + a.normal[2] * b.normal[2]);
  return Math.acos(Math.min(1, d));
}

function tripleDeterminant(a: SurfaceComponent, b: SurfaceComponent, c: SurfaceComponent): number {
  if (a.normal === undefined || b.normal === undefined || c.normal === undefined) return 0;
  const [ax, ay, az] = a.normal; const [bx, by, bz] = b.normal; const [cx, cy, cz] = c.normal;
  return Math.abs(ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx));
}

/**
 * Which faces touch: two components are adjacent when some of their points
 * are within STRUCTURAL_FACE_ADJACENCY_METERS (bounded spatial hash).
 */
function componentAdjacency(
  positions: ArrayLike<number>,
  componentIdByPoint: ArrayLike<number>,
): Set<string> {
  const size = STRUCTURAL_FACE_ADJACENCY_METERS;
  const cells = new Map<string, Array<{ id: number; x: number; y: number; z: number }>>();
  const count = Math.min(componentIdByPoint.length, Math.floor(positions.length / 3));
  for (let index = 0; index < count; index += 1) {
    const id = Number(componentIdByPoint[index]);
    if (id < 0) continue;
    const x = Number(positions[index * 3]); const y = Number(positions[index * 3 + 1]); const z = Number(positions[index * 3 + 2]);
    const key = `${Math.floor(x / size)}:${Math.floor(y / size)}:${Math.floor(z / size)}`;
    const list = cells.get(key) ?? [];
    list.push({ id, x, y, z }); cells.set(key, list);
  }
  const adjacent = new Set<string>();
  for (const [key, list] of cells) {
    const [cx, cy, cz] = key.split(':').map(Number);
    for (let dx = -1; dx <= 1; dx += 1) for (let dy = -1; dy <= 1; dy += 1) for (let dz = -1; dz <= 1; dz += 1) {
      const other = cells.get(`${cx + dx}:${cy + dy}:${cz + dz}`); if (other === undefined) continue;
      for (const a of list) for (const b of other) {
        if (a.id >= b.id) continue;
        const pair = `${a.id}:${b.id}`;
        if (adjacent.has(pair)) continue;
        if (Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z) <= size) adjacent.add(pair);
      }
    }
  }
  return adjacent;
}

/** MAX-254: edge/corner capability of the measured faces (see StructuralFaceCapability). */
export function computeStructuralFaceCapability(
  analysis: Pick<SurfaceComponentAnalysis, 'components' | 'componentIdByPoint'>,
  positions: ArrayLike<number>,
  voxelSizeMeters: number,
): StructuralFaceCapability {
  const faces = analysis.components.filter((component) => component.normal !== undefined && component.pointCount >= 3);
  const faceProgress = faces.map((component) => ({ id: component.id, ...componentStructuralProgress(component, voxelSizeMeters) }));
  const progressById = new Map(faceProgress.map((face) => [face.id, face.progress]));
  const adjacency = analysis.componentIdByPoint === undefined ? new Set<string>() : componentAdjacency(positions, analysis.componentIdByPoint);
  const isAdjacent = (a: SurfaceComponent, b: SurfaceComponent): boolean => adjacency.has(a.id < b.id ? `${a.id}:${b.id}` : `${b.id}:${a.id}`);
  const edgeCapable = (a: SurfaceComponent, b: SurfaceComponent): boolean =>
    isAdjacent(a, b) && normalAngle(a, b) >= STRUCTURAL_EDGE_MIN_ANGLE_RAD;

  let edgeComponents: readonly [SurfaceComponent, SurfaceComponent] | null = null; let edgeProgress = 0;
  let cornerComponents: readonly [SurfaceComponent, SurfaceComponent, SurfaceComponent] | null = null; let cornerProgress = 0;
  for (let i = 0; i < faces.length; i += 1) for (let j = i + 1; j < faces.length; j += 1) {
    const a = faces[i]; const b = faces[j];
    if (!edgeCapable(a, b)) continue;
    const pair = Math.min(progressById.get(a.id) ?? 0, progressById.get(b.id) ?? 0);
    if (pair > edgeProgress) { edgeProgress = pair; edgeComponents = [a, b]; }
    for (let k = j + 1; k < faces.length; k += 1) {
      const c = faces[k];
      if (!edgeCapable(a, c) || !edgeCapable(b, c) || tripleDeterminant(a, b, c) < STRUCTURAL_CORNER_MIN_DETERMINANT) continue;
      const triple = Math.min(pair, progressById.get(c.id) ?? 0);
      if (triple > cornerProgress) { cornerProgress = triple; cornerComponents = [a, b, c]; }
    }
  }
  return { voxelSizeMeters, faceProgress, edgeComponents, edgeProgress, cornerComponents, cornerProgress };
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
  structuralFaces?: StructuralFaceCapability,
): SurfaceComponentReadinessSummary {
  return {
    structuralComponent: bestStructuralComponent(structuralSurface.components, structuralFaces?.voxelSizeMeters)?.component ?? null,
    bestMeshComponent: meshSurface.bestMeshComponent,
    ...(structuralFaces === undefined ? {} : { structuralFaces }),
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
  const faces = surface !== null && surface !== undefined && isCompactSurfaceReadiness(surface) ? surface.structuralFaces : undefined;
  const best = surface === null || surface === undefined
    ? null
    : isCompactSurfaceReadiness(surface)
      ? (surface.structuralComponent === null ? null : { component: surface.structuralComponent, ...componentStructuralProgress(surface.structuralComponent, faces?.voxelSizeMeters) })
      : bestStructuralComponent(surface.components);
  if (best === null) {
    return { progress: 0, ready: false, limitingMetric: 'surface-density', component: null, capability: { surface: false, edge: false, corner: false } };
  }
  const surfaceReady = best.progress >= 1 - 1e-9;
  if (faces === undefined || faces.edgeComponents === null) {
    // One face (or faces that never meet at a usable angle) cannot carry an
    // edge: at most half-way, and the next step is scanning another side.
    const progress = Math.min(best.progress, 1) * 0.5;
    return {
      progress,
      ready: false,
      limitingMetric: surfaceReady ? 'faces' : best.limitingMetric,
      component: best.component,
      capability: { surface: surfaceReady, edge: false, corner: false },
    };
  }
  // The weaker face of the best adjacent pair limits the edge.
  const [first, second] = faces.edgeComponents;
  const weaker = faces.faceProgress.find((face) => face.id === first.id)!.progress <= faces.faceProgress.find((face) => face.id === second.id)!.progress ? first : second;
  const weakerProgress = faces.faceProgress.find((face) => face.id === weaker.id)!;
  const edgeReady = faces.edgeProgress >= 1 - 1e-9;
  return {
    progress: clamp01(faces.edgeProgress),
    ready: edgeReady,
    limitingMetric: edgeReady ? best.limitingMetric : weakerProgress.limitingMetric,
    component: edgeReady ? best.component : weaker,
    capability: { surface: surfaceReady, edge: edgeReady, corner: faces.cornerProgress >= 1 - 1e-9 },
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

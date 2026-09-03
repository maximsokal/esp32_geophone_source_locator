import {
  estimateSurfaceFeatureComplexity,
  featureAwareCellQuota,
} from './surfaceFeatureDensity';
import { metricFeatureProbeRadius } from './metricFeatureProbe';
import {
  classifyDepthNeighbourhood,
  clamp01,
  crossVector,
  depthToleranceMeters,
  lowDiscrepancySample2D,
  orientNormalToward,
  packInitialView,
  registerIndependentView,
  surfaceRenderConfidence,
  type ProjectVector,
} from './surfelEvidence';

export type ProjectPoint = readonly [number, number, number];
type VoxelIndexKey = number | string;

export interface PointCloudBounds {
  min: ProjectPoint;
  max: ProjectPoint;
}

export interface ScanPointCloudSnapshot {
  /** Final promoted + spatially supported surface points in project coordinates. */
  positions: Float32Array;
  confidence: Float32Array;
  normals: Float32Array;
  pointCount: number;
  /** Retained hypotheses that did not make the final stable surface. */
  tentativePositions: Float32Array;
  tentativeConfidence: Float32Array;
  tentativePointCount: number;
  rawPointCount: number;
  bounds: PointCloudBounds | null;
  fullBounds: PointCloudBounds | null;
  voxelSizeMeters: number;
  maxDepthMeters: number;
  depthFrameCount: number;
  sampledDepthFrames: number;
  capacityReached: boolean;
  maxPointCapacity: number;
  boundsTrimFraction: number;
  coverageCellCount: number;
  coverageCellSizeMeters: number;
  adaptivePointsPerCell: number;
  memoryBudgetPoints: number;
  candidateSampleCount: number;
  discardedSampleCount: number;
  replacementCount: number;
  rebalanceEvictionCount: number;
  coverageRejectedCount: number;
  promotedPointCount: number;
  readyCoverageCellCount: number;
  integrationKeyframes: number;
  polishFrames: number;
  residualRejectedSamples: number;
  staleTentativePruned: number;
  averageStableResidualMeters: number;
}

export interface StructuralGuidePointCloudSnapshot {
  /** Promoted surface hypotheses only; no occupancy or robust-bounds work. */
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  pointCount: number;
  keyframeCount: number;
  voxelSizeMeters: number;
}

export interface LiveCoverageSnapshot {
  /** Positions converted to active WebXR reference-space coordinates. */
  positions: Float32Array;
  /** Per-surface confidence, deliberately independent of coverage readiness. */
  confidence: Float32Array;
  pointCount: number;
  active: boolean;
  visibleCoverageCells: number;
  readyCoverageCells: number;
  totalCoverageCells: number;
  minimumVisibleFrameHits: number;
  targetFrameHits: number;
  averageCandidatesPerSampledFrame: number;
  promotedPointCount: number;
  averageCoverageConfidence: number;
}

export interface DepthSampleView {
  projectionMatrix: ArrayLike<number>;
  transform: { matrix: ArrayLike<number> };
}

export interface CpuDepthInformationLike {
  width: number;
  height: number;
  getDepthInMeters(x: number, y: number): number;
  projectionMatrix?: ArrayLike<number>;
  transform?: { matrix: ArrayLike<number> };
}

export interface DepthFusionFrameContext {
  keyframeId: number;
  evidenceKeyframe: boolean;
  frameWeight: number;
  cameraProjectPosition: ProjectPoint;
}

export interface DepthFusionResult {
  changedPoints: number;
  acceptedSamples: number;
  createdPoints: number;
  promotedPoints: number;
  rejectedResiduals: number;
  /** Extra center probes issued around raw depth discontinuities this frame. */
  edgeSupersampleProbes: number;
  /** Extra center probes issued only around physically measured complex surface cells. */
  featureSupersampleProbes: number;
}

interface RepresentativePoint {
  id: string;
  x: number;
  y: number;
  z: number;
  normalX: number;
  normalY: number;
  normalZ: number;
  normalConfidence: number;
  weight: number;
  acceptedSamples: number;
  independentViews: number;
  viewCodesPacked: number;
  viewCodeCount: number;
  lastProcessedKeyframe: number;
  lastEvidenceKeyframe: number;
  lastSeenEpoch: number;
  lastEvidenceEpoch: number;
  varianceX: number;
  varianceY: number;
  varianceZ: number;
  residualEwma: number;
  viewDirX: number;
  viewDirY: number;
  viewDirZ: number;
  maxViewAngleRadians: number;
  contradictionWeight: number;
  promoted: boolean;
  priority: number;
  voxelKey: string;
  voxelIndexKey: VoxelIndexKey;
  coverageKey: string;
}

interface CoverageCell {
  representatives: Map<string, RepresentativePoint>;
  lastEvidenceKeyframe: number;
  evidenceKeyframes: number;
  firstCameraX: number;
  firstCameraY: number;
  firstCameraZ: number;
  hasFirstCamera: boolean;
  maxBaselineMeters: number;
}

interface CellQuality {
  confidence: number;
  visible: boolean;
  ready: boolean;
  promotedPoints: number;
}

interface LocalQuality {
  accepted: boolean;
  score: number;
  edge: boolean;
  dominantAxis: 'u' | 'v' | 'none';
}

interface SurfaceNeighbourhood {
  quality: LocalQuality;
  left: number | null;
  right: number | null;
  up: number | null;
  down: number | null;
}

interface SurfaceNormalEstimate {
  normal: ProjectVector;
  confidence: number;
}

const DEFAULT_VOXEL_SIZE_METERS = 0.04;
const DEFAULT_MAX_DEPTH_METERS = 5.0;
const DEFAULT_MEMORY_BUDGET_POINTS = 60000;
const COVERAGE_CELL_METERS = 0.30;
const MAX_POINTS_PER_COVERAGE_CELL = 28;
const MIN_POINTS_PER_COVERAGE_CELL = 1;
export const MAX_SURFELS_PER_FINE_VOXEL = 3;

// Keep roughly the old 40x22 compute budget but remove its permanent lattice.
const BASE_DEPTH_SAMPLES_PER_FRAME = 880;
// Extra physical probes are hard-capped per sampled XR frame. The previous
// one-level recursion was bounded in depth but not in count: a pathological
// discontinuity-rich frame could issue two children for nearly every base probe.
export const MAX_EDGE_SUPERSAMPLE_PROBES_PER_FRAME = 64;
export const MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME = 96;
// One complex/noisy 30-cm coverage cell must not monopolize all 96 feature probes.
// Six opposite probe pairs are enough to densify one local feature in one frame
// while leaving budget for at least eight independent cells across the object.
export const MAX_FEATURE_SUPERSAMPLE_PROBES_PER_CELL_PER_FRAME = 12;
const FEATURE_PROBE_MIN_REPRESENTATIVES = 6;
const FEATURE_PROBE_COMPLEXITY_THRESHOLD = 0.22;
const GOLDEN_ANGLE_RADIANS = Math.PI * (3 - Math.sqrt(5));
const SAMPLE_MARGIN = 0.025;
const MIN_DEPTH_METERS = 0.18;
const EDGE_PROBE_PIXEL_OFFSET = 0.75;
const NORMAL_PROBE_PIXEL_OFFSET = 1.5;
const NORMAL_COMPATIBILITY_ABS_DOT = Math.cos((50 * Math.PI) / 180);
const PACKED_VOXEL_AXIS_OFFSET = 65536;
const PACKED_VOXEL_AXIS_RADIX = 131072;

const MAX_FUSION_WEIGHT = 12;
const MIN_OBSERVATION_WEIGHT = 0.025;
const PROMOTION_MIN_INDEPENDENT_VIEWS = 2;
const PROMOTION_MIN_FUSION_WEIGHT = 0.75;
const PROMOTION_MIN_VIEW_ANGLE_RADIANS = (8 * Math.PI) / 180;
const TARGET_INDEPENDENT_VIEWS = 3;
const CELL_READY_MIN_PROMOTED = 4;
const CELL_READY_BASELINE_METERS = 0.10;
const CELL_TARGET_BASELINE_METERS = 0.22;
const TENTATIVE_MAX_KEYFRAME_AGE = 7;
const TENTATIVE_PRUNE_INTERVAL_KEYFRAMES = 5;
const FINAL_SUPPORT_CELL_METERS = 0.12;
const MIN_NEIGHBOUR_SUPPORT = 2;
const MIN_FILTERED_POINTS = 80;
const ROBUST_BOUNDS_TRIM_FRACTION = 0.01;
const MIN_POINTS_FOR_ROBUST_BOUNDS = 1000;

// Live guide appears after genuine camera motion creates a second integration
// keyframe, but BEFORE promotion. Tentative red/yellow hypotheses therefore show
// the operator where evidence is weak instead of hiding everything until green.
const LIVE_GUIDE_MIN_KEYFRAMES = 2;
const LIVE_GUIDE_MIN_RETAINED = 120;
const LIVE_GUIDE_MIN_COVERAGE_CELLS = 5;
const LIVE_GUIDE_POINT_BUDGET = 12000;
const LIVE_GUIDE_MAX_POINTS_PER_CELL = 12;

function finite(value: number): boolean { return Number.isFinite(value); }
function clamp(value: number, min: number, max: number): number { return Math.min(max, Math.max(min, value)); }
function gridCoordinate(value: number, cellSizeMeters: number): number { return Math.floor(value / cellSizeMeters); }
function coordinateKey(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }
function packedVoxelIndexKey(x: number, y: number, z: number): VoxelIndexKey {
  if (
    x >= -PACKED_VOXEL_AXIS_OFFSET && x < PACKED_VOXEL_AXIS_OFFSET &&
    y >= -PACKED_VOXEL_AXIS_OFFSET && y < PACKED_VOXEL_AXIS_OFFSET &&
    z >= -PACKED_VOXEL_AXIS_OFFSET && z < PACKED_VOXEL_AXIS_OFFSET
  ) {
    return (
      ((x + PACKED_VOXEL_AXIS_OFFSET) * PACKED_VOXEL_AXIS_RADIX + (y + PACKED_VOXEL_AXIS_OFFSET)) *
      PACKED_VOXEL_AXIS_RADIX +
      (z + PACKED_VOXEL_AXIS_OFFSET)
    );
  }
  // The normal project-scale path is a collision-free exact integer (< 2^51).
  // Keep a string fallback so even pathological coordinates retain old semantics.
  return coordinateKey(x, y, z);
}
function cellKey(x: number, y: number, z: number, cellSizeMeters: number): string {
  return coordinateKey(gridCoordinate(x, cellSizeMeters), gridCoordinate(y, cellSizeMeters), gridCoordinate(z, cellSizeMeters));
}
function voxelCoordinates(x: number, y: number, z: number, size: number): readonly [number, number, number] {
  return [gridCoordinate(x, size), gridCoordinate(y, size), gridCoordinate(z, size)];
}
function voxelKey(x: number, y: number, z: number, size: number): string {
  const [vx, vy, vz] = voxelCoordinates(x, y, z, size);
  return coordinateKey(vx, vy, vz);
}
function voxelIndexKey(x: number, y: number, z: number, size: number): VoxelIndexKey {
  return packedVoxelIndexKey(
    gridCoordinate(x, size),
    gridCoordinate(y, size),
    gridCoordinate(z, size),
  );
}
function hashVoxel(x: number, y: number, z: number): number {
  let hash = 0x811c9dc5;
  hash = Math.imul(hash ^ x, 0x01000193);
  hash = Math.imul(hash ^ y, 0x01000193);
  hash = Math.imul(hash ^ z, 0x01000193);
  hash ^= hash >>> 16;
  hash = Math.imul(hash, 0x7feb352d);
  hash ^= hash >>> 15;
  hash = Math.imul(hash, 0x846ca68b);
  hash ^= hash >>> 16;
  return hash >>> 0;
}
function voxelPriority(x: number, y: number, z: number, size: number): number {
  const [vx, vy, vz] = voxelCoordinates(x, y, z, size);
  return (hashVoxel(vx, vy, vz) + 1) / 4294967297;
}
function representativeResidual(point: RepresentativePoint): number {
  const statistical = Math.sqrt(Math.max(0, (point.varianceX + point.varianceY + point.varianceZ) / 3));
  return Math.max(statistical, point.residualEwma * 0.55);
}
function representativeQuality(point: RepresentativePoint): number {
  return surfaceRenderConfidence({
    promoted: point.promoted,
    independentViews: point.independentViews,
    weight: point.weight,
    residualMeters: representativeResidual(point),
    maxViewAngleRadians: point.maxViewAngleRadians,
    contradictionWeight: point.contradictionWeight,
    normalConfidence: point.normalConfidence,
  });
}
function representativeScore(point: RepresentativePoint): number {
  const quality = Math.max(0.06, representativeQuality(point));
  const diversity = 1 + Math.min(point.independentViews, 4) * 0.35;
  return point.priority / (quality * diversity);
}
function mergeGateMeters(depthMeters: number, point: RepresentativePoint | null): number {
  const rangeGate = clamp(0.028 + 0.0024 * depthMeters * depthMeters, 0.034, 0.085);
  if (point === null) return rangeGate;
  const statistical = representativeResidual(point) * 2.5 + 0.010;
  return clamp(Math.max(rangeGate, statistical), 0.034, 0.095);
}
function promotionResidualLimitMeters(depthMeters: number): number {
  return clamp(0.018 + 0.0019 * depthMeters * depthMeters, 0.022, 0.062);
}
function rangeWeight(depthMeters: number): number { return 1 / (1 + 0.12 * depthMeters * depthMeters); }
function sortedAxis(positions: ArrayLike<number>, axis: 0 | 1 | 2): number[] {
  const values = new Array<number>(positions.length / 3);
  let out = 0;
  for (let index = axis; index < positions.length; index += 3) values[out++] = positions[index];
  values.sort((a, b) => a - b);
  return values;
}
function percentileIndex(count: number, fraction: number): number {
  if (count <= 1) return 0;
  return clamp(Math.round((count - 1) * fraction), 0, count - 1);
}
function supportNeighbourCount(support: ReadonlyMap<string, number>, x: number, y: number, z: number): number {
  const cx = gridCoordinate(x, FINAL_SUPPORT_CELL_METERS);
  const cy = gridCoordinate(y, FINAL_SUPPORT_CELL_METERS);
  const cz = gridCoordinate(z, FINAL_SUPPORT_CELL_METERS);
  let count = 0;
  for (let dx = -1; dx <= 1; dx += 1) for (let dy = -1; dy <= 1; dy += 1) for (let dz = -1; dz <= 1; dz += 1) {
    count += support.get(coordinateKey(cx + dx, cy + dy, cz + dz)) ?? 0;
  }
  return count;
}
function cameraDirection(x: number, y: number, z: number, camera: ProjectPoint): ProjectVector {
  const dx = camera[0] - x;
  const dy = camera[1] - y;
  const dz = camera[2] - z;
  const length = Math.hypot(dx, dy, dz) || 1;
  return [dx / length, dy / length, dz / length];
}
function safeDepth(depth: CpuDepthInformationLike, u: number, v: number): number | null {
  if (u <= 0 || u >= 1 || v <= 0 || v >= 1) return null;
  const value = depth.getDepthInMeters(u, v);
  return finite(value) && value >= MIN_DEPTH_METERS ? value : null;
}
function readSurfaceNeighbourhood(
  depth: CpuDepthInformationLike,
  u: number,
  v: number,
  center: number,
): SurfaceNeighbourhood {
  const du = NORMAL_PROBE_PIXEL_OFFSET / Math.max(1, depth.width);
  const dv = NORMAL_PROBE_PIXEL_OFFSET / Math.max(1, depth.height);
  const left = safeDepth(depth, u - du, v);
  const right = safeDepth(depth, u + du, v);
  const up = safeDepth(depth, u, v - dv);
  const down = safeDepth(depth, u, v + dv);
  return {
    quality: classifyDepthNeighbourhood(center, left, right, up, down),
    left,
    right,
    up,
    down,
  };
}

function unprojectProjectPoint(
  u: number,
  v: number,
  depthMeters: number,
  p00: number,
  p11: number,
  p20: number,
  p21: number,
  worldFromView: ArrayLike<number>,
): ProjectPoint | null {
  const ndcX = u * 2 - 1;
  const ndcY = 1 - v * 2;
  const viewX = ((ndcX + p20) * depthMeters) / p00;
  const viewY = ((ndcY + p21) * depthMeters) / p11;
  const viewZ = -depthMeters;
  const worldX = Number(worldFromView[0]) * viewX + Number(worldFromView[4]) * viewY + Number(worldFromView[8]) * viewZ + Number(worldFromView[12]);
  const worldY = Number(worldFromView[1]) * viewX + Number(worldFromView[5]) * viewY + Number(worldFromView[9]) * viewZ + Number(worldFromView[13]);
  const worldZ = Number(worldFromView[2]) * viewX + Number(worldFromView[6]) * viewY + Number(worldFromView[10]) * viewZ + Number(worldFromView[14]);
  if (![worldX, worldY, worldZ].every(finite)) return null;
  return [worldX, -worldZ, worldY];
}

function subtract(a: ProjectPoint, b: ProjectPoint): ProjectVector {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function estimateSurfaceNormal(
  depth: CpuDepthInformationLike,
  u: number,
  v: number,
  centerDepth: number,
  center: ProjectPoint,
  neighbours: SurfaceNeighbourhood,
  p00: number,
  p11: number,
  p20: number,
  p21: number,
  worldFromView: ArrayLike<number>,
  camera: ProjectPoint,
): SurfaceNormalEstimate | null {
  const du = NORMAL_PROBE_PIXEL_OFFSET / Math.max(1, depth.width);
  const dv = NORMAL_PROBE_PIXEL_OFFSET / Math.max(1, depth.height);
  const tolerance = depthToleranceMeters(centerDepth) * 1.45;

  const projectNeighbour = (su: number, sv: number, d: number | null): ProjectPoint | null => {
    if (d === null || Math.abs(d - centerDepth) > tolerance) return null;
    return unprojectProjectPoint(su, sv, d, p00, p11, p20, p21, worldFromView);
  };

  const left = projectNeighbour(u - du, v, neighbours.left);
  const right = projectNeighbour(u + du, v, neighbours.right);
  const up = projectNeighbour(u, v - dv, neighbours.up);
  const down = projectNeighbour(u, v + dv, neighbours.down);

  let tangentU: ProjectVector | null = null;
  let tangentV: ProjectVector | null = null;
  if (left !== null && right !== null) tangentU = subtract(right, left);
  else if (right !== null) tangentU = subtract(right, center);
  else if (left !== null) tangentU = subtract(center, left);

  if (up !== null && down !== null) tangentV = subtract(down, up);
  else if (down !== null) tangentV = subtract(down, center);
  else if (up !== null) tangentV = subtract(center, up);

  if (tangentU === null || tangentV === null) return null;
  if (Math.hypot(...tangentU) < 1e-5 || Math.hypot(...tangentV) < 1e-5) return null;

  const towardCamera = cameraDirection(center[0], center[1], center[2], camera);
  const normal = orientNormalToward(crossVector(tangentU, tangentV), towardCamera);
  const supportCount = Number(left !== null) + Number(right !== null) + Number(up !== null) + Number(down !== null);
  return { normal, confidence: clamp01(0.22 + supportCount * 0.19) };
}

export class DepthPointCloudAccumulator {
  private readonly voxelSizeMeters: number;
  private readonly memoryBudgetPoints: number;
  private maxDepthMeters: number;
  private readonly coverageCells = new Map<string, CoverageCell>();
  private readonly voxelIndex = new Map<VoxelIndexKey, RepresentativePoint[]>();
  private readonly representatives = new Map<string, RepresentativePoint>();
  private representativeCount = 0;
  private promotedRepresentativeCount = 0;
  /** Fair-share base quota. Feature cells may retain more, but never exceed global memory. */
  private adaptiveQuota = MAX_POINTS_PER_COVERAGE_CELL;
  private candidateSampleCount = 0;
  private discardedSampleCount = 0;
  private replacementCount = 0;
  private rebalanceEvictionCount = 0;
  private coverageRejectedCount = 0;
  private residualRejectedSamples = 0;
  private staleTentativePruned = 0;
  private sampleEpoch = 0;
  private integrationKeyframes = 0;
  private polishFrames = 0;
  private lastPrunedKeyframe = 0;
  private nextId = 1;

  constructor(
    voxelSizeMeters = DEFAULT_VOXEL_SIZE_METERS,
    memoryBudgetPoints = DEFAULT_MEMORY_BUDGET_POINTS,
    maxDepthMeters = DEFAULT_MAX_DEPTH_METERS,
  ) {
    this.voxelSizeMeters = voxelSizeMeters;
    this.memoryBudgetPoints = Math.max(1000, Math.floor(memoryBudgetPoints));
    this.maxDepthMeters = maxDepthMeters;
  }

  reset(): void {
    this.coverageCells.clear(); this.voxelIndex.clear(); this.representatives.clear();
    this.representativeCount = 0; this.promotedRepresentativeCount = 0;
    this.adaptiveQuota = MAX_POINTS_PER_COVERAGE_CELL;
    this.candidateSampleCount = 0; this.discardedSampleCount = 0; this.replacementCount = 0;
    this.rebalanceEvictionCount = 0; this.coverageRejectedCount = 0; this.residualRejectedSamples = 0;
    this.staleTentativePruned = 0; this.sampleEpoch = 0; this.integrationKeyframes = 0; this.polishFrames = 0;
    this.lastPrunedKeyframe = 0; this.nextId = 1;
  }
  setMaxDepthMeters(value: number): void { if (finite(value)) this.maxDepthMeters = clamp(value, 1, 8); }
  get maxDepth(): number { return this.maxDepthMeters; }
  get pointCount(): number { return this.representativeCount; }
  get memoryBudget(): number { return this.memoryBudgetPoints; }
  get coverageCount(): number { return this.coverageCells.size; }
  get pointsPerCoverageCell(): number { return this.adaptiveQuota; }
  get voxelSize(): number { return this.voxelSizeMeters; }
  get keyframeCount(): number { return this.integrationKeyframes; }
  get polishFrameCount(): number { return this.polishFrames; }
  get rejectedResidualCount(): number { return this.residualRejectedSamples; }
  get promotedCount(): number { return this.promotedRepresentativeCount; }
  get averageCandidatesPerSampledFrame(): number {
    return this.sampleEpoch > 0 ? this.candidateSampleCount / this.sampleEpoch : 0;
  }

  sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
    context: DepthFusionFrameContext,
  ): DepthFusionResult {
    const projection = depth.projectionMatrix !== undefined && depth.projectionMatrix.length >= 16
      ? depth.projectionMatrix : fallbackView.projectionMatrix;
    const worldFromView = depth.transform !== undefined && depth.transform.matrix.length >= 16
      ? depth.transform.matrix : fallbackView.transform.matrix;
    if (projection.length < 16 || worldFromView.length < 16) return this.emptyResult();

    const p00 = Number(projection[0]); const p11 = Number(projection[5]);
    const p20 = Number(projection[8]); const p21 = Number(projection[9]);
    if (!finite(p00) || !finite(p11) || Math.abs(p00) < 1e-6 || Math.abs(p11) < 1e-6) {
      return this.emptyResult();
    }

    this.sampleEpoch += 1;
    if (context.evidenceKeyframe) this.integrationKeyframes += 1; else this.polishFrames += 1;
    const phase = context.keyframeId * 131 + this.sampleEpoch * 17 + (context.evidenceKeyframe ? 0 : 71);
    let changedPoints = 0; let acceptedSamples = 0; let createdPoints = 0; let promotedPoints = 0;
    let edgeSupersampleProbes = 0; let featureSupersampleProbes = 0; let featureProbeSequence = 0;
    const residualBefore = this.residualRejectedSamples;
    // A cell's normal-family score changes slowly inside one frame. Cache at most
    // one clustering pass per touched cell so feature-directed sampling remains
    // bounded in both depth reads and CPU work.
    const featureScoreCache = new Map<string, number>();
    const featureProbeCountsByCell = new Map<string, number>();

    const featureScoreForProject = (project: ProjectPoint): number => {
      const key = cellKey(project[0], project[1], project[2], COVERAGE_CELL_METERS);
      const cached = featureScoreCache.get(key);
      if (cached !== undefined) return cached;
      const cell = this.coverageCells.get(key);
      if (cell === undefined || cell.representatives.size < FEATURE_PROBE_MIN_REPRESENTATIVES) {
        featureScoreCache.set(key, 0);
        return 0;
      }
      const score = this.cellFeatureComplexity(cell);
      featureScoreCache.set(key, score);
      return score;
    };

    const integrateAt = (u: number, v: number, allowAdaptiveExpansion: boolean): LocalQuality | null => {
      if (u <= SAMPLE_MARGIN || u >= 1 - SAMPLE_MARGIN || v <= SAMPLE_MARGIN || v >= 1 - SAMPLE_MARGIN) return null;
      const depthMeters = depth.getDepthInMeters(u, v);
      if (!finite(depthMeters) || depthMeters < MIN_DEPTH_METERS || depthMeters > this.maxDepthMeters) return null;
      const neighbourhood = readSurfaceNeighbourhood(depth, u, v, depthMeters);
      const local = neighbourhood.quality;
      if (!local.accepted) { this.discardedSampleCount += 1; return local; }

      const project = unprojectProjectPoint(u, v, depthMeters, p00, p11, p20, p21, worldFromView);
      if (project === null) return local;
      const normalEstimate = estimateSurfaceNormal(
        depth, u, v, depthMeters, project, neighbourhood, p00, p11, p20, p21, worldFromView,
        context.cameraProjectPosition,
      );

      this.candidateSampleCount += 1;
      const edgeWeight = local.edge ? 0.84 : 1;
      const normalWeight = normalEstimate === null ? 0.74 : 0.82 + normalEstimate.confidence * 0.18;
      const observationWeight = clamp01(context.frameWeight) * local.score * edgeWeight * normalWeight * rangeWeight(depthMeters);
      if (observationWeight < MIN_OBSERVATION_WEIGHT) { this.discardedSampleCount += 1; return local; }

      const outcome = this.observePoint(
        project[0], project[1], project[2], depthMeters, observationWeight,
        normalEstimate, context,
      );
      if (outcome.accepted) acceptedSamples += 1;
      if (outcome.changed) changedPoints += 1;
      if (outcome.created) createdPoints += 1;
      if (outcome.promoted) promotedPoints += 1;

      if (allowAdaptiveExpansion && outcome.accepted) {
        const featureCoverageKey = cellKey(project[0], project[1], project[2], COVERAGE_CELL_METERS);
        const cellFeatureProbes = featureProbeCountsByCell.get(featureCoverageKey) ?? 0;
        const complexity = featureScoreForProject(project);
        if (
          complexity >= FEATURE_PROBE_COMPLEXITY_THRESHOLD &&
          featureSupersampleProbes + 2 <= MAX_FEATURE_SUPERSAMPLE_PROBES_PER_FRAME &&
          cellFeatureProbes + 2 <= MAX_FEATURE_SUPERSAMPLE_PROBES_PER_CELL_PER_FRAME
        ) {
          // Opposite points on a deterministic rotating stencil provide local
          // density without introducing a permanent image-grid pattern. This is
          // still physical depth only: learned/native plane priors cannot enter
          // this accumulator or request probes. Per-cell fairness prevents one
          // noisy/complex patch from consuming the entire frame-wide budget.
          const probeRadius = metricFeatureProbeRadius({
            depthMeters,
            voxelSizeMeters: this.voxelSizeMeters,
            projectionScaleX: p00,
            projectionScaleY: p11,
            depthWidth: depth.width,
            depthHeight: depth.height,
          });
          if (probeRadius !== null) {
            const angle = (phase * 0.17320508075688773 + featureProbeSequence * GOLDEN_ANGLE_RADIANS) % (Math.PI * 2);
            featureProbeSequence += 1;
            const du = Math.cos(angle) * probeRadius.normalizedRadiusU;
            const dv = Math.sin(angle) * probeRadius.normalizedRadiusV;
            featureSupersampleProbes += 2;
            featureProbeCountsByCell.set(featureCoverageKey, cellFeatureProbes + 2);
            integrateAt(u - du, v - dv, false);
            integrateAt(u + du, v + dv, false);
            return local;
          }
        }
      }

      // A raw discontinuity remains useful for a narrow fallback supersample,
      // but now has a true frame-wide budget rather than only a recursion-depth
      // bound. Feature-confirmed cells above take priority over generic edges.
      if (
        allowAdaptiveExpansion && local.edge && local.dominantAxis !== 'none' &&
        edgeSupersampleProbes + 2 <= MAX_EDGE_SUPERSAMPLE_PROBES_PER_FRAME
      ) {
        const du = EDGE_PROBE_PIXEL_OFFSET / Math.max(1, depth.width);
        const dv = EDGE_PROBE_PIXEL_OFFSET / Math.max(1, depth.height);
        edgeSupersampleProbes += 2;
        if (local.dominantAxis === 'u') {
          integrateAt(u - du, v, false); integrateAt(u + du, v, false);
        } else {
          integrateAt(u, v - dv, false); integrateAt(u, v + dv, false);
        }
      }
      return local;
    };

    for (let index = 0; index < BASE_DEPTH_SAMPLES_PER_FRAME; index += 1) {
      const [u, v] = lowDiscrepancySample2D(index, SAMPLE_MARGIN, phase);
      integrateAt(u, v, true);
    }

    if (
      context.evidenceKeyframe &&
      context.keyframeId - this.lastPrunedKeyframe >= TENTATIVE_PRUNE_INTERVAL_KEYFRAMES
    ) {
      this.pruneStaleTentative(context.keyframeId);
      this.lastPrunedKeyframe = context.keyframeId;
    }
    return {
      changedPoints,
      acceptedSamples,
      createdPoints,
      promotedPoints,
      rejectedResiduals: this.residualRejectedSamples - residualBefore,
      edgeSupersampleProbes,
      featureSupersampleProbes,
    };
  }

  liveCoverageSnapshot(): LiveCoverageSnapshot {
    const promotedPointCount = this.promotedRepresentativeCount;
    const active =
      this.integrationKeyframes >= LIVE_GUIDE_MIN_KEYFRAMES &&
      this.representativeCount >= LIVE_GUIDE_MIN_RETAINED &&
      this.coverageCells.size >= LIVE_GUIDE_MIN_COVERAGE_CELLS;
    let visibleCoverageCells = 0; let readyCoverageCells = 0; let confidenceSum = 0;
    const qualities = new Map<CoverageCell, CellQuality>();
    for (const cell of this.coverageCells.values()) {
      const quality = this.cellQuality(cell); qualities.set(cell, quality);
      if (quality.visible) { visibleCoverageCells += 1; confidenceSum += quality.confidence; }
      if (quality.ready) readyCoverageCells += 1;
    }
    const averageCoverageConfidence = visibleCoverageCells > 0 ? confidenceSum / visibleCoverageCells : 0;
    if (!active || visibleCoverageCells === 0) return {
      positions: new Float32Array(0), confidence: new Float32Array(0), pointCount: 0, active,
      visibleCoverageCells, readyCoverageCells, totalCoverageCells: this.coverageCells.size,
      minimumVisibleFrameHits: PROMOTION_MIN_INDEPENDENT_VIEWS, targetFrameHits: TARGET_INDEPENDENT_VIEWS,
      averageCandidatesPerSampledFrame: this.averageCandidatesPerSampledFrame,
      promotedPointCount, averageCoverageConfidence,
    };

    const perCellBudget = clamp(
      Math.floor(LIVE_GUIDE_POINT_BUDGET / visibleCoverageCells),
      1,
      LIVE_GUIDE_MAX_POINTS_PER_CELL,
    );
    const positions = new Float32Array(LIVE_GUIDE_POINT_BUDGET * 3);
    const confidence = new Float32Array(LIVE_GUIDE_POINT_BUDGET);
    let pointCount = 0;
    for (const cell of this.coverageCells.values()) {
      const quality = qualities.get(cell); if (quality === undefined || !quality.visible) continue;
      const ranked = [...cell.representatives.values()].sort((a, b) => representativeScore(a) - representativeScore(b));
      const take = Math.min(
        Math.max(1, Math.round(perCellBudget * (0.45 + 0.55 * Math.max(0.2, quality.confidence)))),
        ranked.length,
      );
      for (let index = 0; index < take && pointCount < LIVE_GUIDE_POINT_BUDGET; index += 1) {
        const point = ranked[index]; const offset = pointCount * 3;
        positions[offset] = point.x;
        positions[offset + 1] = point.z;
        positions[offset + 2] = -point.y;
        confidence[pointCount] = representativeQuality(point);
        pointCount += 1;
      }
    }
    return {
      positions: positions.subarray(0, pointCount * 3),
      confidence: confidence.subarray(0, pointCount),
      pointCount,
      active,
      visibleCoverageCells,
      readyCoverageCells,
      totalCoverageCells: this.coverageCells.size,
      minimumVisibleFrameHits: PROMOTION_MIN_INDEPENDENT_VIEWS,
      targetFrameHits: TARGET_INDEPENDENT_VIEWS,
      averageCandidatesPerSampledFrame: this.averageCandidatesPerSampledFrame,
      promotedPointCount,
      averageCoverageConfidence,
    };
  }

  structuralGuideSnapshot(): StructuralGuidePointCloudSnapshot {
    // The live guide must not call the full final snapshot: final snapshot also
    // builds tentative arrays, support maps, robust sorted bounds and occupancy
    // in the wrapper. Here we copy only the promoted surfel state required by
    // the off-main-thread plane/corner detector.
    const count = this.promotedRepresentativeCount;
    const positions = new Float32Array(count * 3);
    const normals = new Float32Array(count * 3);
    const confidence = new Float32Array(count);
    let write = 0;
    for (const point of this.representatives.values()) {
      if (!point.promoted) continue;
      const offset = write * 3;
      positions[offset] = point.x;
      positions[offset + 1] = point.y;
      positions[offset + 2] = point.z;
      normals[offset] = point.normalX;
      normals[offset + 1] = point.normalY;
      normals[offset + 2] = point.normalZ;
      confidence[write] = representativeQuality(point);
      write += 1;
    }
    return {
      positions: write === count ? positions : positions.subarray(0, write * 3),
      normals: write === count ? normals : normals.subarray(0, write * 3),
      confidence: write === count ? confidence : confidence.subarray(0, write),
      pointCount: write,
      keyframeCount: this.integrationKeyframes,
      voxelSizeMeters: this.voxelSizeMeters,
    };
  }

  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
    const all = [...this.representatives.values()];
    const promoted = all.filter((point) => point.promoted);
    const support = new Map<string, number>();
    for (const point of promoted) {
      const key = cellKey(point.x, point.y, point.z, FINAL_SUPPORT_CELL_METERS);
      support.set(key, (support.get(key) ?? 0) + 1);
    }
    const supported = promoted.filter((point) =>
      point.independentViews >= TARGET_INDEPENDENT_VIEWS ||
      supportNeighbourCount(support, point.x, point.y, point.z) >= MIN_NEIGHBOUR_SUPPORT,
    );
    const finalPoints = supported.length >= MIN_FILTERED_POINTS ? supported : promoted;
    const finalIds = new Set(finalPoints.map((point) => point.id));
    const tentative = all.filter((point) => !finalIds.has(point.id));

    const finalPositions = new Float32Array(finalPoints.length * 3);
    const finalConfidence = new Float32Array(finalPoints.length);
    const finalNormals = new Float32Array(finalPoints.length * 3);
    let residualSum = 0;
    finalPoints.forEach((point, index) => {
      const offset = index * 3;
      finalPositions[offset] = point.x;
      finalPositions[offset + 1] = point.y;
      finalPositions[offset + 2] = point.z;
      finalConfidence[index] = representativeQuality(point);
      finalNormals[offset] = point.normalX;
      finalNormals[offset + 1] = point.normalY;
      finalNormals[offset + 2] = point.normalZ;
      residualSum += representativeResidual(point);
    });

    const tentativePositions = new Float32Array(tentative.length * 3);
    const tentativeConfidence = new Float32Array(tentative.length);
    tentative.forEach((point, index) => {
      const offset = index * 3;
      tentativePositions[offset] = point.x;
      tentativePositions[offset + 1] = point.y;
      tentativePositions[offset + 2] = point.z;
      tentativeConfidence[index] = representativeQuality(point);
    });

    let readyCoverageCellCount = 0;
    for (const cell of this.coverageCells.values()) if (this.cellQuality(cell).ready) readyCoverageCellCount += 1;
    const fullBounds = this.computeFullBounds(finalPositions);
    const bounds = this.computeRobustBounds(finalPositions) ?? fullBounds;
    return {
      positions: finalPositions,
      confidence: finalConfidence,
      normals: finalNormals,
      pointCount: finalPoints.length,
      tentativePositions,
      tentativeConfidence,
      tentativePointCount: tentative.length,
      rawPointCount: all.length,
      bounds,
      fullBounds,
      voxelSizeMeters: this.voxelSizeMeters,
      maxDepthMeters: this.maxDepthMeters,
      depthFrameCount,
      sampledDepthFrames,
      capacityReached: this.coverageRejectedCount > 0,
      maxPointCapacity: this.memoryBudgetPoints,
      boundsTrimFraction: finalPoints.length >= MIN_POINTS_FOR_ROBUST_BOUNDS ? ROBUST_BOUNDS_TRIM_FRACTION : 0,
      coverageCellCount: this.coverageCells.size,
      coverageCellSizeMeters: COVERAGE_CELL_METERS,
      adaptivePointsPerCell: this.adaptiveQuota,
      memoryBudgetPoints: this.memoryBudgetPoints,
      candidateSampleCount: this.candidateSampleCount,
      discardedSampleCount: this.discardedSampleCount,
      replacementCount: this.replacementCount,
      rebalanceEvictionCount: this.rebalanceEvictionCount,
      coverageRejectedCount: this.coverageRejectedCount,
      promotedPointCount: promoted.length,
      readyCoverageCellCount,
      integrationKeyframes: this.integrationKeyframes,
      polishFrames: this.polishFrames,
      residualRejectedSamples: this.residualRejectedSamples,
      staleTentativePruned: this.staleTentativePruned,
      averageStableResidualMeters: finalPoints.length > 0 ? residualSum / finalPoints.length : 0,
    };
  }

  private observePoint(
    x: number,
    y: number,
    z: number,
    depthMeters: number,
    observationWeight: number,
    normalEstimate: SurfaceNormalEstimate | null,
    context: DepthFusionFrameContext,
  ): { accepted: boolean; changed: boolean; created: boolean; promoted: boolean } {
    const nearby = this.findNearbyRepresentative(x, y, z, normalEstimate);
    if (nearby !== null) {
      const residual = Math.hypot(x - nearby.x, y - nearby.y, z - nearby.z);
      if (residual <= mergeGateMeters(depthMeters, nearby)) {
        const before = nearby.promoted;
        this.fuseRepresentative(
          nearby, x, y, z, depthMeters, observationWeight, normalEstimate, context, residual,
        );
        return { accepted: true, changed: true, created: false, promoted: !before && nearby.promoted };
      }
      this.residualRejectedSamples += 1;
      if (!context.evidenceKeyframe) {
        this.discardedSampleCount += 1;
        return { accepted: false, changed: false, created: false, promoted: false };
      }
      this.registerContradictionIfApplicable(nearby, x, y, z, residual, depthMeters, context);
    }
    if (!context.evidenceKeyframe) {
      this.discardedSampleCount += 1;
      return { accepted: false, changed: false, created: false, promoted: false };
    }

    const coverageKey = cellKey(x, y, z, COVERAGE_CELL_METERS);
    let cell = this.coverageCells.get(coverageKey);
    if (cell === undefined) {
      if (this.coverageCells.size >= this.memoryBudgetPoints) {
        this.coverageRejectedCount += 1; this.discardedSampleCount += 1;
        return { accepted: false, changed: false, created: false, promoted: false };
      }
      cell = this.createCoverageCell();
      this.coverageCells.set(coverageKey, cell);
      this.updateAdaptiveQuota();
    }
    const key = voxelKey(x, y, z, this.voxelSizeMeters);
    const indexKey = voxelIndexKey(x, y, z, this.voxelSizeMeters);
    const bucket = this.voxelIndex.get(indexKey) ?? [];
    if (bucket.length >= MAX_SURFELS_PER_FINE_VOXEL) {
      let worst = bucket[0];
      let worstScore = representativeScore(worst);
      for (let index = 1; index < bucket.length; index += 1) {
        const score = representativeScore(bucket[index]);
        if (score > worstScore) { worst = bucket[index]; worstScore = score; }
      }
      const probeQuality = this.candidatePriority(x, y, z);
      if (probeQuality >= worstScore) {
        this.discardedSampleCount += 1;
        return { accepted: false, changed: false, created: false, promoted: false };
      }
      this.removeRepresentative(worst); this.replacementCount += 1;
    }

    const direction = cameraDirection(x, y, z, context.cameraProjectPosition);
    const initialView = packInitialView(direction);
    const normal = normalEstimate?.normal ?? ([0, 0, 1] as const);
    const id = `${key}#${this.nextId++}`;
    const candidate: RepresentativePoint = {
      id, x, y, z,
      normalX: normal[0], normalY: normal[1], normalZ: normal[2],
      normalConfidence: normalEstimate?.confidence ?? 0,
      weight: Math.min(MAX_FUSION_WEIGHT, observationWeight),
      acceptedSamples: 1,
      independentViews: 1,
      viewCodesPacked: initialView.packed,
      viewCodeCount: initialView.count,
      lastProcessedKeyframe: context.keyframeId,
      lastEvidenceKeyframe: context.keyframeId,
      lastSeenEpoch: this.sampleEpoch,
      lastEvidenceEpoch: this.sampleEpoch,
      varianceX: 0, varianceY: 0, varianceZ: 0,
      residualEwma: mergeGateMeters(depthMeters, null) * 0.45,
      viewDirX: direction[0], viewDirY: direction[1], viewDirZ: direction[2],
      maxViewAngleRadians: 0,
      contradictionWeight: 0,
      promoted: false,
      priority: voxelPriority(x, y, z, this.voxelSizeMeters),
      voxelKey: key,
      voxelIndexKey: indexKey,
      coverageKey,
    };
    if (!this.insertCandidate(cell, candidate)) {
      this.discardedSampleCount += 1;
      return { accepted: false, changed: false, created: false, promoted: false };
    }
    this.recordCellEvidence(cell, context);
    return { accepted: true, changed: true, created: true, promoted: false };
  }

  private fuseRepresentative(
    point: RepresentativePoint,
    x: number,
    y: number,
    z: number,
    depthMeters: number,
    observationWeight: number,
    normalEstimate: SurfaceNormalEstimate | null,
    context: DepthFusionFrameContext,
    residual: number,
  ): void {
    const priorWeight = Math.min(MAX_FUSION_WEIGHT, Math.max(0.001, point.weight));
    const incoming = clamp(observationWeight, MIN_OBSERVATION_WEIGHT, 1);
    const effectivePrior = Math.min(priorWeight, MAX_FUSION_WEIGHT - incoming * 0.5);
    const total = effectivePrior + incoming;
    const alpha = incoming / total;
    const dx = x - point.x; const dy = y - point.y; const dz = z - point.z;
    point.x += alpha * dx; point.y += alpha * dy; point.z += alpha * dz;
    point.varianceX = (1 - alpha) * (point.varianceX + alpha * dx * dx);
    point.varianceY = (1 - alpha) * (point.varianceY + alpha * dy * dy);
    point.varianceZ = (1 - alpha) * (point.varianceZ + alpha * dz * dz);
    point.residualEwma += (residual - point.residualEwma) * (context.evidenceKeyframe ? 0.24 : 0.12);
    point.weight = Math.min(MAX_FUSION_WEIGHT, priorWeight + incoming);
    point.acceptedSamples += 1;
    point.lastSeenEpoch = this.sampleEpoch;
    point.contradictionWeight *= 0.84;

    if (normalEstimate !== null) {
      const beta = clamp01(alpha * (0.35 + normalEstimate.confidence * 0.65));
      // Surface-normal sign is view-dependent here: the fresh estimate is
      // oriented toward the current camera, while the stored surfel may have
      // been created from the opposite side. Association is intentionally
      // sign-invariant, therefore fusion must first align both normals to the
      // same hemisphere or n + (-n) can collapse into an unstable direction.
      const normalSign = (
        point.normalX * normalEstimate.normal[0] +
        point.normalY * normalEstimate.normal[1] +
        point.normalZ * normalEstimate.normal[2]
      ) < 0 ? -1 : 1;
      const nx = point.normalX * (1 - beta) + normalEstimate.normal[0] * normalSign * beta;
      const ny = point.normalY * (1 - beta) + normalEstimate.normal[1] * normalSign * beta;
      const nz = point.normalZ * (1 - beta) + normalEstimate.normal[2] * normalSign * beta;
      const length = Math.hypot(nx, ny, nz) || 1;
      point.normalX = nx / length; point.normalY = ny / length; point.normalZ = nz / length;
      point.normalConfidence = clamp01(
        point.normalConfidence + (normalEstimate.confidence - point.normalConfidence) * Math.max(0.08, alpha),
      );
    }

    if (context.evidenceKeyframe && point.lastProcessedKeyframe !== context.keyframeId) {
      point.lastProcessedKeyframe = context.keyframeId;
      const direction = cameraDirection(point.x, point.y, point.z, context.cameraProjectPosition);
      const evidence = registerIndependentView(point.viewCodesPacked, point.viewCodeCount, direction);
      if (evidence.independent) {
        point.viewCodesPacked = evidence.packed;
        point.viewCodeCount = evidence.count;
        point.independentViews = evidence.count;
        point.maxViewAngleRadians = Math.max(
          point.maxViewAngleRadians,
          Math.acos(clamp(
            point.viewDirX * direction[0] + point.viewDirY * direction[1] + point.viewDirZ * direction[2],
            -1, 1,
          )),
        );
        point.lastEvidenceKeyframe = context.keyframeId;
        point.lastEvidenceEpoch = this.sampleEpoch;
        const cell = this.coverageCells.get(point.coverageKey);
        if (cell !== undefined) this.recordCellEvidence(cell, context);
      }
    }

    const residualNow = representativeResidual(point);
    if (
      !point.promoted &&
      point.independentViews >= PROMOTION_MIN_INDEPENDENT_VIEWS &&
      point.maxViewAngleRadians >= PROMOTION_MIN_VIEW_ANGLE_RADIANS &&
      point.weight >= PROMOTION_MIN_FUSION_WEIGHT &&
      residualNow <= promotionResidualLimitMeters(depthMeters) &&
      point.contradictionWeight < 1.5
    ) {
      point.promoted = true;
      this.promotedRepresentativeCount += 1;
    }
  }

  private registerContradictionIfApplicable(
    point: RepresentativePoint,
    x: number,
    y: number,
    z: number,
    residual: number,
    depthMeters: number,
    context: DepthFusionFrameContext,
  ): void {
    if (!context.evidenceKeyframe || point.lastProcessedKeyframe === context.keyframeId) return;
    if (voxelKey(x, y, z, this.voxelSizeMeters) !== point.voxelKey) return;
    if (residual < mergeGateMeters(depthMeters, point) * 1.7) return;
    point.lastProcessedKeyframe = context.keyframeId;
    point.contradictionWeight = Math.min(6, point.contradictionWeight + Math.max(0.25, context.frameWeight));
    if (point.promoted && point.contradictionWeight >= 2.75) {
      point.promoted = false;
      this.promotedRepresentativeCount = Math.max(0, this.promotedRepresentativeCount - 1);
    }
  }

  private findNearbyRepresentative(
    x: number,
    y: number,
    z: number,
    normalEstimate: SurfaceNormalEstimate | null,
  ): RepresentativePoint | null {
    const size = this.voxelSizeMeters;
    const vx = gridCoordinate(x, size);
    const vy = gridCoordinate(y, size);
    const vz = gridCoordinate(z, size);
    const normal = normalEstimate !== null && normalEstimate.confidence >= 0.35
      ? normalEstimate.normal
      : null;
    let best: RepresentativePoint | null = null;
    let bestDistanceSquared = Number.POSITIVE_INFINITY;
    for (let dx = -1; dx <= 1; dx += 1) for (let dy = -1; dy <= 1; dy += 1) for (let dz = -1; dz <= 1; dz += 1) {
      const bucket = this.voxelIndex.get(packedVoxelIndexKey(vx + dx, vy + dy, vz + dz));
      if (bucket === undefined) continue;
      for (const point of bucket) {
        const ddx = x - point.x;
        const ddy = y - point.y;
        const ddz = z - point.z;
        const distanceSquared = ddx * ddx + ddy * ddy + ddz * ddz;
        // Compatibility work cannot change the winner when this point is already
        // farther than the best compatible candidate found in the same scan order.
        if (distanceSquared >= bestDistanceSquared) continue;
        if (normal !== null && point.normalConfidence >= 0.35) {
          // Both stored surfel normals and fresh normal estimates are normalized.
          // Compare their absolute dot product directly instead of allocating
          // temporary vectors, re-normalizing twice and evaluating acos().
          const absDot = Math.abs(
            point.normalX * normal[0] + point.normalY * normal[1] + point.normalZ * normal[2],
          );
          if (absDot < NORMAL_COMPATIBILITY_ABS_DOT) continue;
        }
        best = point;
        bestDistanceSquared = distanceSquared;
      }
    }
    return best;
  }

  private candidatePriority(x: number, y: number, z: number): number {
    return voxelPriority(x, y, z, this.voxelSizeMeters) / 0.08;
  }
  private createCoverageCell(): CoverageCell {
    return {
      representatives: new Map(), lastEvidenceKeyframe: -1, evidenceKeyframes: 0,
      firstCameraX: 0, firstCameraY: 0, firstCameraZ: 0, hasFirstCamera: false,
      maxBaselineMeters: 0,
    };
  }
  private recordCellEvidence(cell: CoverageCell, context: DepthFusionFrameContext): void {
    if (!context.evidenceKeyframe || cell.lastEvidenceKeyframe === context.keyframeId) return;
    cell.lastEvidenceKeyframe = context.keyframeId; cell.evidenceKeyframes += 1;
    const camera = context.cameraProjectPosition;
    if (!cell.hasFirstCamera) {
      cell.firstCameraX = camera[0]; cell.firstCameraY = camera[1]; cell.firstCameraZ = camera[2];
      cell.hasFirstCamera = true; return;
    }
    cell.maxBaselineMeters = Math.max(
      cell.maxBaselineMeters,
      Math.hypot(camera[0] - cell.firstCameraX, camera[1] - cell.firstCameraY, camera[2] - cell.firstCameraZ),
    );
  }
  private cellQuality(cell: CoverageCell): CellQuality {
    let promotedPoints = 0; let promotedQualitySum = 0; let allQualitySum = 0; let maxAngle = 0;
    for (const point of cell.representatives.values()) {
      const quality = representativeQuality(point);
      allQualitySum += quality;
      if (point.promoted) {
        promotedPoints += 1; promotedQualitySum += quality;
        maxAngle = Math.max(maxAngle, point.maxViewAngleRadians);
      }
    }
    const averagePointQuality = promotedPoints > 0
      ? promotedQualitySum / promotedPoints
      : (cell.representatives.size > 0 ? allQualitySum / cell.representatives.size : 0);
    const densityScore = clamp01(promotedPoints / CELL_READY_MIN_PROMOTED);
    const baselineScore = clamp01(cell.maxBaselineMeters / CELL_TARGET_BASELINE_METERS);
    const angleScore = clamp01(maxAngle / ((18 * Math.PI) / 180));
    const geometryDiversity = Math.max(baselineScore, angleScore * 0.75);
    const confidence = clamp01(densityScore * 0.40 + geometryDiversity * 0.34 + averagePointQuality * 0.26);
    const visible = cell.representatives.size > 0 && cell.evidenceKeyframes >= 1;
    const ready =
      promotedPoints >= CELL_READY_MIN_PROMOTED &&
      cell.evidenceKeyframes >= TARGET_INDEPENDENT_VIEWS &&
      cell.maxBaselineMeters >= CELL_READY_BASELINE_METERS &&
      confidence >= 0.72;
    return { confidence, visible, ready, promotedPoints };
  }

  private cellFeatureComplexity(cell: CoverageCell, candidate?: RepresentativePoint): number {
    if (candidate === undefined) return estimateSurfaceFeatureComplexity(cell.representatives.values()).score;
    const existing = cell.representatives.values();
    const withCandidate: Iterable<RepresentativePoint> = {
      *[Symbol.iterator](): Iterator<RepresentativePoint> {
        yield* existing;
        yield candidate;
      },
    };
    return estimateSurfaceFeatureComplexity(withCandidate).score;
  }

  private cellRetentionQuota(cell: CoverageCell, candidate?: RepresentativePoint, baseQuota = this.adaptiveQuota): number {
    return featureAwareCellQuota(baseQuota, this.cellFeatureComplexity(cell, candidate));
  }

  private insertCandidate(cell: CoverageCell, candidate: RepresentativePoint): boolean {
    // The normal-family/curvature analysis is intentionally lazy. Most cells are
    // below their fair-share quota and global memory is not saturated, so their
    // normal sets never need to be clustered in this hot path.
    let projectedComplexity: number | null = null;
    const getProjectedComplexity = (): number => {
      if (projectedComplexity === null) projectedComplexity = this.cellFeatureComplexity(cell, candidate);
      return projectedComplexity;
    };

    let localQuota = this.adaptiveQuota;
    if (cell.representatives.size >= this.adaptiveQuota) {
      localQuota = featureAwareCellQuota(this.adaptiveQuota, getProjectedComplexity());
    }
    if (cell.representatives.size >= localQuota) {
      const worst = this.findWorstRepresentative(cell);
      if (worst === null || representativeScore(candidate) >= worst.score) return false;
      this.removeRepresentative(worst.point); this.replacementCount += 1;
    } else if (!this.ensureGlobalSlot(candidate.coverageKey, getProjectedComplexity)) return false;

    cell.representatives.set(candidate.id, candidate);
    this.representatives.set(candidate.id, candidate);
    const bucket = this.voxelIndex.get(candidate.voxelIndexKey) ?? [];
    bucket.push(candidate); this.voxelIndex.set(candidate.voxelIndexKey, bucket);
    this.representativeCount += 1; return true;
  }
  private desiredAdaptiveQuota(): number {
    return clamp(
      Math.floor(this.memoryBudgetPoints / Math.max(1, this.coverageCells.size)),
      MIN_POINTS_PER_COVERAGE_CELL,
      MAX_POINTS_PER_COVERAGE_CELL,
    );
  }
  private updateAdaptiveQuota(): void {
    const desired = this.desiredAdaptiveQuota();
    if (desired >= this.adaptiveQuota) return;
    this.adaptiveQuota = desired; this.trimAllCellsToQuota(desired);
  }
  private trimAllCellsToQuota(baseQuota: number): void {
    for (const cell of this.coverageCells.values()) {
      const localQuota = this.cellRetentionQuota(cell, undefined, baseQuota);
      if (cell.representatives.size <= localQuota) continue;
      const ranked = [...cell.representatives.values()].sort((a, b) => representativeScore(a) - representativeScore(b));
      for (let i = localQuota; i < ranked.length; i += 1) {
        this.removeRepresentative(ranked[i]); this.rebalanceEvictionCount += 1;
      }
    }
  }
  private ensureGlobalSlot(preferredCoverageKey: string, preferredComplexity: () => number): boolean {
    if (this.representativeCount < this.memoryBudgetPoints) return true;
    this.updateAdaptiveQuota();
    if (this.representativeCount < this.memoryBudgetPoints) return true;

    // Only after global saturation do we pay for normal-family analysis. Flat
    // requested cells skip donor complexity entirely because there is no feature
    // advantage to preserve; quality/surplus ranking remains the old fast path.
    const requestedComplexity = preferredComplexity();
    const useFeatureBias = requestedComplexity >= 0.15;
    let donor: RepresentativePoint | null = null;
    let donorEvictionScore = Number.NEGATIVE_INFINITY;
    for (const [coverageKey, cell] of this.coverageCells) {
      if (coverageKey === preferredCoverageKey || cell.representatives.size <= MIN_POINTS_PER_COVERAGE_CELL) continue;
      const worst = this.findWorstRepresentative(cell);
      if (worst === null) continue;
      const donorComplexity = useFeatureBias ? this.cellFeatureComplexity(cell) : 0;
      const complexityAdvantage = useFeatureBias
        ? clamp(requestedComplexity - donorComplexity, -1, 1)
        : 0;
      // More-complex requested cells preferentially borrow from flatter cells.
      // Existing point quality remains the primary eviction signal, so this is a
      // bounded bias rather than a rule that can protect bad feature evidence.
      const featureBias = 1 + complexityAdvantage * 0.65;
      const aboveBase = Math.max(0, cell.representatives.size - this.adaptiveQuota);
      const surplusBias = 1 + Math.min(0.35, aboveBase * 0.035);
      const evictionScore = worst.score * featureBias * surplusBias;
      if (evictionScore > donorEvictionScore) {
        donor = worst.point;
        donorEvictionScore = evictionScore;
      }
    }
    if (donor === null) { this.coverageRejectedCount += 1; return false; }
    this.removeRepresentative(donor); this.rebalanceEvictionCount += 1; return true;
  }
  private findWorstRepresentative(cell: CoverageCell): { point: RepresentativePoint; score: number } | null {
    let worst: RepresentativePoint | null = null; let score = Number.NEGATIVE_INFINITY;
    for (const point of cell.representatives.values()) {
      const current = representativeScore(point);
      if (current > score) { worst = point; score = current; }
    }
    return worst === null ? null : { point: worst, score };
  }
  private removeRepresentative(point: RepresentativePoint): void {
    const cell = this.coverageCells.get(point.coverageKey); cell?.representatives.delete(point.id);
    this.representatives.delete(point.id);
    const bucket = this.voxelIndex.get(point.voxelIndexKey);
    if (bucket !== undefined) {
      const index = bucket.indexOf(point);
      if (index >= 0) bucket.splice(index, 1);
      if (bucket.length === 0) this.voxelIndex.delete(point.voxelIndexKey);
    }
    if (point.promoted) {
      this.promotedRepresentativeCount = Math.max(0, this.promotedRepresentativeCount - 1);
    }
    this.representativeCount = Math.max(0, this.representativeCount - 1);
  }
  private pruneStaleTentative(currentKeyframe: number): void {
    const remove: RepresentativePoint[] = [];
    for (const point of this.representatives.values()) {
      if (!point.promoted && currentKeyframe - point.lastEvidenceKeyframe > TENTATIVE_MAX_KEYFRAME_AGE) remove.push(point);
    }
    for (const point of remove) { this.removeRepresentative(point); this.staleTentativePruned += 1; }
  }
  private computeFullBounds(positions: ArrayLike<number>): PointCloudBounds | null {
    if (positions.length < 3) return null;
    let minX = Infinity, minY = Infinity, minZ = Infinity;
    let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
    for (let i = 0; i < positions.length; i += 3) {
      const x = positions[i], y = positions[i + 1], z = positions[i + 2];
      minX = Math.min(minX, x); minY = Math.min(minY, y); minZ = Math.min(minZ, z);
      maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); maxZ = Math.max(maxZ, z);
    }
    return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
  }
  private computeRobustBounds(positions: ArrayLike<number>): PointCloudBounds | null {
    const count = positions.length / 3;
    if (count < MIN_POINTS_FOR_ROBUST_BOUNDS) return null;
    const x = sortedAxis(positions, 0), y = sortedAxis(positions, 1), z = sortedAxis(positions, 2);
    const low = percentileIndex(count, ROBUST_BOUNDS_TRIM_FRACTION);
    const high = percentileIndex(count, 1 - ROBUST_BOUNDS_TRIM_FRACTION);
    return { min: [x[low], y[low], z[low]], max: [x[high], y[high], z[high]] };
  }
  private emptyResult(): DepthFusionResult {
    return {
      changedPoints: 0,
      acceptedSamples: 0,
      createdPoints: 0,
      promotedPoints: 0,
      rejectedResiduals: 0,
      edgeSupersampleProbes: 0,
      featureSupersampleProbes: 0,
    };
  }
}

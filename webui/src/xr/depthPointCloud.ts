export type ProjectPoint = readonly [number, number, number];

export interface PointCloudBounds {
  min: ProjectPoint;
  max: ProjectPoint;
}

export interface ScanPointCloudSnapshot {
  positions: Float32Array;
  pointCount: number;
  /** All retained representatives, including still-tentative surface hypotheses. */
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

export interface LiveCoverageSnapshot {
  /** Positions converted back into WebXR reference-space X/Y/Z for rendering. */
  positions: Float32Array;
  /** Robust surface confidence in [0, 1]. */
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
  transform: {
    matrix: ArrayLike<number>;
  };
}

export interface CpuDepthInformationLike {
  width: number;
  height: number;
  getDepthInMeters(x: number, y: number): number;
  projectionMatrix?: ArrayLike<number>;
  transform?: {
    matrix: ArrayLike<number>;
  };
}

export interface DepthFusionFrameContext {
  /** Monotonically increasing only for geometrically novel integration keyframes. */
  keyframeId: number;
  /** False for low-weight stationary/redundant polishing frames. */
  evidenceKeyframe: boolean;
  /** Motion-derived quality in [0, 1]. */
  frameWeight: number;
  /** Camera origin in GeoLocator project coordinates. */
  cameraProjectPosition: ProjectPoint;
}

export interface DepthFusionResult {
  changedPoints: number;
  acceptedSamples: number;
  createdPoints: number;
  promotedPoints: number;
  rejectedResiduals: number;
}

interface RepresentativePoint {
  x: number;
  y: number;
  z: number;
  /** Bounded fusion weight. Capping prevents an old surface from becoming immutable. */
  weight: number;
  acceptedSamples: number;
  independentViews: number;
  lastEvidenceKeyframe: number;
  lastSeenEpoch: number;
  lastEvidenceEpoch: number;
  varianceX: number;
  varianceY: number;
  varianceZ: number;
  viewDirX: number;
  viewDirY: number;
  viewDirZ: number;
  maxViewAngleRadians: number;
  promoted: boolean;
  priority: number;
  voxelKey: string;
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

interface LocalDepthQuality {
  accepted: boolean;
  score: number;
}

interface CellQuality {
  confidence: number;
  visible: boolean;
  ready: boolean;
  promotedPoints: number;
}

const DEFAULT_VOXEL_SIZE_METERS = 0.04;
const DEFAULT_MAX_DEPTH_METERS = 5.0;
const DEFAULT_MEMORY_BUDGET_POINTS = 60000;
const COVERAGE_CELL_METERS = 0.30;
const MAX_POINTS_PER_COVERAGE_CELL = 28;
const MIN_POINTS_PER_COVERAGE_CELL = 1;

const SAMPLE_COLUMNS = 40;
const SAMPLE_ROWS = 22;
const SAMPLE_MARGIN = 0.025;
const MIN_DEPTH_METERS = 0.18;

const MAX_FUSION_WEIGHT = 12;
const MIN_OBSERVATION_WEIGHT = 0.025;
const PROMOTION_MIN_INDEPENDENT_VIEWS = 2;
const PROMOTION_MIN_FUSION_WEIGHT = 0.75;
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

const LIVE_GUIDE_MIN_KEYFRAMES = 3;
const LIVE_GUIDE_MIN_PROMOTED = 50;
const LIVE_GUIDE_MIN_COVERAGE_CELLS = 10;
const LIVE_GUIDE_POINT_BUDGET = 12000;
const LIVE_GUIDE_MAX_POINTS_PER_CELL = 12;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

function gridCoordinate(value: number, cellSizeMeters: number): number {
  return Math.floor(value / cellSizeMeters);
}

function coordinateKey(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function cellKey(x: number, y: number, z: number, cellSizeMeters: number): string {
  return coordinateKey(
    gridCoordinate(x, cellSizeMeters),
    gridCoordinate(y, cellSizeMeters),
    gridCoordinate(z, cellSizeMeters),
  );
}

function voxelCoordinates(
  x: number,
  y: number,
  z: number,
  voxelSizeMeters: number,
): readonly [number, number, number] {
  return [
    gridCoordinate(x, voxelSizeMeters),
    gridCoordinate(y, voxelSizeMeters),
    gridCoordinate(z, voxelSizeMeters),
  ];
}

function voxelKey(x: number, y: number, z: number, voxelSizeMeters: number): string {
  const [vx, vy, vz] = voxelCoordinates(x, y, z, voxelSizeMeters);
  return coordinateKey(vx, vy, vz);
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

function voxelPriority(x: number, y: number, z: number, voxelSizeMeters: number): number {
  const [vx, vy, vz] = voxelCoordinates(x, y, z, voxelSizeMeters);
  return (hashVoxel(vx, vy, vz) + 1) / 4294967297;
}

function representativeResidual(point: RepresentativePoint): number {
  return Math.sqrt(
    Math.max(0, (point.varianceX + point.varianceY + point.varianceZ) / 3),
  );
}

function representativeQuality(point: RepresentativePoint): number {
  const views = clamp01(point.independentViews / TARGET_INDEPENDENT_VIEWS);
  const residual = representativeResidual(point);
  const residualScore = 1 - clamp01(residual / 0.065);
  const weightScore = clamp01(point.weight / 4);
  const promotedBonus = point.promoted ? 1 : 0.25;
  return clamp01(
    promotedBonus * (views * 0.45 + residualScore * 0.35 + weightScore * 0.20),
  );
}

/** Lower is better for adaptive bottom-k retention. */
function representativeScore(point: RepresentativePoint): number {
  const quality = Math.max(0.08, representativeQuality(point));
  const viewBonus = 1 + Math.min(point.independentViews, 4) * 0.35;
  return point.priority / (quality * viewBonus);
}

function mergeGateMeters(depthMeters: number, point: RepresentativePoint | null): number {
  // ARCore documents depth error as increasing approximately quadratically with
  // distance. The gate follows that trend but is capped so far depth cannot
  // merge unrelated surfaces together.
  const rangeGate = clamp(0.032 + 0.0026 * depthMeters * depthMeters, 0.038, 0.10);
  if (point === null) {
    return rangeGate;
  }

  const statisticalGate = representativeResidual(point) * 2.75 + 0.012;
  return clamp(Math.max(rangeGate, statisticalGate), 0.038, 0.115);
}

function promotionResidualLimitMeters(depthMeters: number): number {
  return clamp(0.018 + 0.0019 * depthMeters * depthMeters, 0.022, 0.062);
}

function rangeWeight(depthMeters: number): number {
  return 1 / (1 + 0.12 * depthMeters * depthMeters);
}

function median(values: number[]): number {
  values.sort((a, b) => a - b);
  const middle = Math.floor(values.length / 2);
  return values.length % 2 === 0
    ? (values[middle - 1] + values[middle]) * 0.5
    : values[middle];
}

function localDepthQuality(
  depth: CpuDepthInformationLike,
  u: number,
  v: number,
  centerDepth: number,
): LocalDepthQuality {
  const deltaU = 1.5 / Math.max(1, depth.width);
  const deltaV = 1.5 / Math.max(1, depth.height);
  const neighbours: readonly [number, number][] = [
    [u - deltaU, v],
    [u + deltaU, v],
    [u, v - deltaV],
    [u, v + deltaV],
  ];

  const valid: number[] = [];
  for (const [sampleU, sampleV] of neighbours) {
    if (sampleU <= 0 || sampleU >= 1 || sampleV <= 0 || sampleV >= 1) {
      continue;
    }
    const value = depth.getDepthInMeters(sampleU, sampleV);
    if (finite(value) && value >= MIN_DEPTH_METERS) {
      valid.push(value);
    }
  }

  if (valid.length < 2) {
    return { accepted: true, score: 0.42 };
  }

  const neighbourhoodMedian = median(valid);
  const residual = Math.abs(centerDepth - neighbourhoodMedian);
  const tolerance = clamp(
    0.020 + 0.0040 * centerDepth * centerDepth,
    0.026,
    0.105,
  );

  if (residual > tolerance) {
    return { accepted: false, score: 0 };
  }

  return {
    accepted: true,
    score: clamp01(1 - residual / tolerance),
  };
}

function sortedAxis(positions: readonly number[], axis: 0 | 1 | 2): number[] {
  const values = new Array<number>(positions.length / 3);
  let writeIndex = 0;
  for (let index = axis; index < positions.length; index += 3) {
    values[writeIndex] = positions[index];
    writeIndex += 1;
  }
  values.sort((left, right) => left - right);
  return values;
}

function percentileIndex(count: number, fraction: number): number {
  if (count <= 1) return 0;
  return clamp(Math.round((count - 1) * fraction), 0, count - 1);
}

function supportNeighbourCount(
  support: ReadonlyMap<string, number>,
  x: number,
  y: number,
  z: number,
): number {
  const cx = gridCoordinate(x, FINAL_SUPPORT_CELL_METERS);
  const cy = gridCoordinate(y, FINAL_SUPPORT_CELL_METERS);
  const cz = gridCoordinate(z, FINAL_SUPPORT_CELL_METERS);
  let count = 0;

  for (let dx = -1; dx <= 1; dx += 1) {
    for (let dy = -1; dy <= 1; dy += 1) {
      for (let dz = -1; dz <= 1; dz += 1) {
        count += support.get(coordinateKey(cx + dx, cy + dy, cz + dz)) ?? 0;
      }
    }
  }
  return count;
}

function cameraDirection(
  pointX: number,
  pointY: number,
  pointZ: number,
  camera: ProjectPoint,
): readonly [number, number, number] {
  const dx = camera[0] - pointX;
  const dy = camera[1] - pointY;
  const dz = camera[2] - pointZ;
  const length = Math.hypot(dx, dy, dz) || 1;
  return [dx / length, dy / length, dz / length];
}

function angularDistance(
  ax: number,
  ay: number,
  az: number,
  bx: number,
  by: number,
  bz: number,
): number {
  const dot = clamp(ax * bx + ay * by + az * bz, -1, 1);
  return Math.acos(dot);
}

/**
 * Bounded robust surface accumulator inspired by real-time TSDF/surfel systems.
 *
 * Key rules:
 * - only geometrically novel keyframes may create new surface hypotheses;
 * - redundant/stationary frames may only polish an already matched surface;
 * - fusion weight is capped, so an old voxel never becomes immutable;
 * - residual/variance gates reject observations inconsistent with the current
 *   surface instead of letting repeated noise manufacture confidence;
 * - tentative voxels must be confirmed by independent keyframes or age out;
 * - coverage readiness comes from promoted surface + camera baseline, not from
 *   a raw frame counter;
 * - adaptive spatial quotas keep the global browser-memory budget bounded.
 */
export class DepthPointCloudAccumulator {
  private readonly voxelSizeMeters: number;
  private readonly memoryBudgetPoints: number;
  private maxDepthMeters: number;
  private readonly coverageCells = new Map<string, CoverageCell>();
  private readonly voxelIndex = new Map<string, RepresentativePoint>();
  private representativeCount = 0;
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
    this.coverageCells.clear();
    this.voxelIndex.clear();
    this.representativeCount = 0;
    this.adaptiveQuota = MAX_POINTS_PER_COVERAGE_CELL;
    this.candidateSampleCount = 0;
    this.discardedSampleCount = 0;
    this.replacementCount = 0;
    this.rebalanceEvictionCount = 0;
    this.coverageRejectedCount = 0;
    this.residualRejectedSamples = 0;
    this.staleTentativePruned = 0;
    this.sampleEpoch = 0;
    this.integrationKeyframes = 0;
    this.polishFrames = 0;
    this.lastPrunedKeyframe = 0;
  }

  setMaxDepthMeters(maxDepthMeters: number): void {
    if (finite(maxDepthMeters)) {
      this.maxDepthMeters = clamp(maxDepthMeters, 1.0, 8.0);
    }
  }

  get maxDepth(): number { return this.maxDepthMeters; }
  get pointCount(): number { return this.representativeCount; }
  get memoryBudget(): number { return this.memoryBudgetPoints; }
  get coverageCount(): number { return this.coverageCells.size; }
  get pointsPerCoverageCell(): number { return this.adaptiveQuota; }
  get voxelSize(): number { return this.voxelSizeMeters; }
  get keyframeCount(): number { return this.integrationKeyframes; }
  get polishFrameCount(): number { return this.polishFrames; }
  get rejectedResidualCount(): number { return this.residualRejectedSamples; }

  get promotedCount(): number {
    let count = 0;
    for (const point of this.voxelIndex.values()) {
      if (point.promoted) count += 1;
    }
    return count;
  }

  get averageCandidatesPerSampledFrame(): number {
    return this.sampleEpoch > 0 ? this.candidateSampleCount / this.sampleEpoch : 0;
  }

  sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
    context: DepthFusionFrameContext,
  ): DepthFusionResult {
    const projection =
      depth.projectionMatrix !== undefined && depth.projectionMatrix.length >= 16
        ? depth.projectionMatrix
        : fallbackView.projectionMatrix;
    const worldFromView =
      depth.transform !== undefined && depth.transform.matrix.length >= 16
        ? depth.transform.matrix
        : fallbackView.transform.matrix;

    if (projection.length < 16 || worldFromView.length < 16) {
      return this.emptyResult();
    }

    const p00 = Number(projection[0]);
    const p11 = Number(projection[5]);
    const p20 = Number(projection[8]);
    const p21 = Number(projection[9]);
    if (!finite(p00) || !finite(p11) || Math.abs(p00) < 1e-6 || Math.abs(p11) < 1e-6) {
      return this.emptyResult();
    }

    this.sampleEpoch += 1;
    if (context.evidenceKeyframe) this.integrationKeyframes += 1;
    else this.polishFrames += 1;

    let changedPoints = 0;
    let acceptedSamples = 0;
    let createdPoints = 0;
    let promotedPoints = 0;
    const residualBefore = this.residualRejectedSamples;
    const usableSpan = 1 - SAMPLE_MARGIN * 2;

    for (let row = 0; row < SAMPLE_ROWS; row += 1) {
      const v = SAMPLE_MARGIN + ((row + 0.5) / SAMPLE_ROWS) * usableSpan;
      const ndcY = 1 - v * 2;

      for (let column = 0; column < SAMPLE_COLUMNS; column += 1) {
        const u = SAMPLE_MARGIN + ((column + 0.5) / SAMPLE_COLUMNS) * usableSpan;
        const depthMeters = depth.getDepthInMeters(u, v);
        if (
          !finite(depthMeters) ||
          depthMeters < MIN_DEPTH_METERS ||
          depthMeters > this.maxDepthMeters
        ) {
          continue;
        }

        const local = localDepthQuality(depth, u, v, depthMeters);
        if (!local.accepted) {
          this.discardedSampleCount += 1;
          continue;
        }

        const ndcX = u * 2 - 1;
        const viewX = ((ndcX + p20) * depthMeters) / p00;
        const viewY = ((ndcY + p21) * depthMeters) / p11;
        const viewZ = -depthMeters;

        const worldX =
          Number(worldFromView[0]) * viewX + Number(worldFromView[4]) * viewY +
          Number(worldFromView[8]) * viewZ + Number(worldFromView[12]);
        const worldY =
          Number(worldFromView[1]) * viewX + Number(worldFromView[5]) * viewY +
          Number(worldFromView[9]) * viewZ + Number(worldFromView[13]);
        const worldZ =
          Number(worldFromView[2]) * viewX + Number(worldFromView[6]) * viewY +
          Number(worldFromView[10]) * viewZ + Number(worldFromView[14]);

        if (!finite(worldX) || !finite(worldY) || !finite(worldZ)) continue;

        const projectX = worldX;
        const projectY = -worldZ;
        const projectZ = worldY;
        this.candidateSampleCount += 1;

        const observationWeight =
          clamp01(context.frameWeight) * local.score * rangeWeight(depthMeters);
        if (observationWeight < MIN_OBSERVATION_WEIGHT) {
          this.discardedSampleCount += 1;
          continue;
        }

        const outcome = this.observePoint(
          projectX,
          projectY,
          projectZ,
          depthMeters,
          observationWeight,
          context,
        );
        if (outcome.accepted) acceptedSamples += 1;
        if (outcome.changed) changedPoints += 1;
        if (outcome.created) createdPoints += 1;
        if (outcome.promoted) promotedPoints += 1;
      }
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
    };
  }

  liveCoverageSnapshot(): LiveCoverageSnapshot {
    const promotedPointCount = this.promotedCount;
    const active =
      this.integrationKeyframes >= LIVE_GUIDE_MIN_KEYFRAMES &&
      promotedPointCount >= LIVE_GUIDE_MIN_PROMOTED &&
      this.coverageCells.size >= LIVE_GUIDE_MIN_COVERAGE_CELLS;

    let visibleCoverageCells = 0;
    let readyCoverageCells = 0;
    let confidenceSum = 0;
    const qualities = new Map<CoverageCell, CellQuality>();

    for (const cell of this.coverageCells.values()) {
      const quality = this.cellQuality(cell);
      qualities.set(cell, quality);
      if (quality.visible) {
        visibleCoverageCells += 1;
        confidenceSum += quality.confidence;
      }
      if (quality.ready) readyCoverageCells += 1;
    }

    const averageCoverageConfidence =
      visibleCoverageCells > 0 ? confidenceSum / visibleCoverageCells : 0;

    if (!active || visibleCoverageCells === 0) {
      return {
        positions: new Float32Array(0),
        confidence: new Float32Array(0),
        pointCount: 0,
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

    const perCellBudget = clamp(
      Math.floor(LIVE_GUIDE_POINT_BUDGET / visibleCoverageCells),
      1,
      LIVE_GUIDE_MAX_POINTS_PER_CELL,
    );
    const positions = new Float32Array(LIVE_GUIDE_POINT_BUDGET * 3);
    const confidence = new Float32Array(LIVE_GUIDE_POINT_BUDGET);
    let pointCount = 0;

    for (const cell of this.coverageCells.values()) {
      const quality = qualities.get(cell);
      if (quality === undefined || !quality.visible) continue;

      const ranked = [...cell.representatives.values()]
        .filter((point) => point.promoted || point.independentViews >= 2)
        .sort((left, right) => representativeScore(left) - representativeScore(right));
      if (ranked.length === 0) continue;

      const requested = Math.max(
        1,
        Math.round(perCellBudget * (0.30 + 0.70 * quality.confidence)),
      );
      const take = Math.min(requested, ranked.length);

      for (let index = 0; index < take && pointCount < LIVE_GUIDE_POINT_BUDGET; index += 1) {
        const point = ranked[index];
        const pointConfidence = clamp01(
          quality.confidence * 0.72 + representativeQuality(point) * 0.28,
        );
        const offset = pointCount * 3;
        positions[offset] = point.x;
        positions[offset + 1] = point.z;
        positions[offset + 2] = -point.y;
        confidence[pointCount] = pointConfidence;
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

  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
    const representatives = this.collectRepresentatives();
    const promoted = representatives.filter((point) => point.promoted);
    const support = new Map<string, number>();

    for (const point of promoted) {
      const key = cellKey(point.x, point.y, point.z, FINAL_SUPPORT_CELL_METERS);
      support.set(key, (support.get(key) ?? 0) + 1);
    }

    const spatiallySupported = promoted.filter((point) =>
      point.independentViews >= TARGET_INDEPENDENT_VIEWS ||
      supportNeighbourCount(support, point.x, point.y, point.z) >= MIN_NEIGHBOUR_SUPPORT,
    );
    const finalPoints =
      spatiallySupported.length >= MIN_FILTERED_POINTS ? spatiallySupported : promoted;

    const finalPositions = new Array<number>(finalPoints.length * 3);
    let writeIndex = 0;
    let residualSum = 0;
    for (const point of finalPoints) {
      finalPositions[writeIndex] = point.x;
      finalPositions[writeIndex + 1] = point.y;
      finalPositions[writeIndex + 2] = point.z;
      writeIndex += 3;
      residualSum += representativeResidual(point);
    }

    let readyCoverageCellCount = 0;
    for (const cell of this.coverageCells.values()) {
      if (this.cellQuality(cell).ready) readyCoverageCellCount += 1;
    }

    const fullBounds = this.computeFullBounds(finalPositions);
    const bounds = this.computeRobustBounds(finalPositions) ?? fullBounds;

    return {
      positions: Float32Array.from(finalPositions),
      pointCount: finalPoints.length,
      rawPointCount: representatives.length,
      bounds,
      fullBounds,
      voxelSizeMeters: this.voxelSizeMeters,
      maxDepthMeters: this.maxDepthMeters,
      depthFrameCount,
      sampledDepthFrames,
      capacityReached: this.coverageRejectedCount > 0,
      maxPointCapacity: this.memoryBudgetPoints,
      boundsTrimFraction:
        finalPoints.length >= MIN_POINTS_FOR_ROBUST_BOUNDS
          ? ROBUST_BOUNDS_TRIM_FRACTION
          : 0,
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
      averageStableResidualMeters:
        finalPoints.length > 0 ? residualSum / finalPoints.length : 0,
    };
  }

  private observePoint(
    x: number,
    y: number,
    z: number,
    depthMeters: number,
    observationWeight: number,
    context: DepthFusionFrameContext,
  ): { accepted: boolean; changed: boolean; created: boolean; promoted: boolean } {
    const nearby = this.findNearbyRepresentative(x, y, z);

    if (nearby !== null) {
      const residual = Math.hypot(x - nearby.x, y - nearby.y, z - nearby.z);
      if (residual <= mergeGateMeters(depthMeters, nearby)) {
        const promotedBefore = nearby.promoted;
        this.fuseRepresentative(
          nearby,
          x,
          y,
          z,
          depthMeters,
          observationWeight,
          context,
        );
        return {
          accepted: true,
          changed: true,
          created: false,
          promoted: !promotedBefore && nearby.promoted,
        };
      }

      // A residual outside the merge gate is a depth discontinuity, not proof
      // that the sample itself is invalid. Redundant polish frames are not
      // allowed to create a second surface hypothesis, but a novel keyframe may
      // seed the candidate's own free voxel so nearby foreground/background
      // surfaces are preserved rather than erased at object silhouettes.
      this.residualRejectedSamples += 1;
      if (!context.evidenceKeyframe) {
        this.discardedSampleCount += 1;
        return { accepted: false, changed: false, created: false, promoted: false };
      }
    }

    // This is the central anti-saturation rule: a redundant or stationary frame
    // cannot manufacture new geometry. It may only refine a surface that an
    // independent keyframe already established.
    if (!context.evidenceKeyframe) {
      this.discardedSampleCount += 1;
      return { accepted: false, changed: false, created: false, promoted: false };
    }

    const coverageKey = cellKey(x, y, z, COVERAGE_CELL_METERS);
    let cell = this.coverageCells.get(coverageKey);
    if (cell === undefined) {
      if (this.coverageCells.size >= this.memoryBudgetPoints) {
        this.coverageRejectedCount += 1;
        this.discardedSampleCount += 1;
        return { accepted: false, changed: false, created: false, promoted: false };
      }
      cell = this.createCoverageCell();
      this.coverageCells.set(coverageKey, cell);
      this.updateAdaptiveQuota();
    }

    const key = voxelKey(x, y, z, this.voxelSizeMeters);
    if (this.voxelIndex.has(key)) {
      this.discardedSampleCount += 1;
      return { accepted: false, changed: false, created: false, promoted: false };
    }

    const [viewDirX, viewDirY, viewDirZ] = cameraDirection(
      x, y, z, context.cameraProjectPosition,
    );
    const candidate: RepresentativePoint = {
      x,
      y,
      z,
      weight: Math.min(MAX_FUSION_WEIGHT, observationWeight),
      acceptedSamples: 1,
      independentViews: 1,
      lastEvidenceKeyframe: context.keyframeId,
      lastSeenEpoch: this.sampleEpoch,
      lastEvidenceEpoch: this.sampleEpoch,
      varianceX: 0,
      varianceY: 0,
      varianceZ: 0,
      viewDirX,
      viewDirY,
      viewDirZ,
      maxViewAngleRadians: 0,
      promoted: false,
      priority: voxelPriority(x, y, z, this.voxelSizeMeters),
      voxelKey: key,
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
    context: DepthFusionFrameContext,
  ): void {
    const priorWeight = Math.min(MAX_FUSION_WEIGHT, Math.max(0.001, point.weight));
    const incoming = clamp(observationWeight, MIN_OBSERVATION_WEIGHT, 1);
    // Even at max weight we keep a finite update gain. This is deliberate:
    // "saturated" surfaces keep self-polishing instead of freezing forever.
    const effectivePrior = Math.min(priorWeight, MAX_FUSION_WEIGHT - incoming * 0.5);
    const total = effectivePrior + incoming;
    const alpha = incoming / total;

    const dx = x - point.x;
    const dy = y - point.y;
    const dz = z - point.z;
    point.x += alpha * dx;
    point.y += alpha * dy;
    point.z += alpha * dz;
    point.varianceX = (1 - alpha) * (point.varianceX + alpha * dx * dx);
    point.varianceY = (1 - alpha) * (point.varianceY + alpha * dy * dy);
    point.varianceZ = (1 - alpha) * (point.varianceZ + alpha * dz * dz);
    point.weight = Math.min(MAX_FUSION_WEIGHT, priorWeight + incoming);
    point.acceptedSamples += 1;
    point.lastSeenEpoch = this.sampleEpoch;

    if (
      context.evidenceKeyframe &&
      point.lastEvidenceKeyframe !== context.keyframeId
    ) {
      const [vx, vy, vz] = cameraDirection(
        point.x, point.y, point.z, context.cameraProjectPosition,
      );
      point.maxViewAngleRadians = Math.max(
        point.maxViewAngleRadians,
        angularDistance(point.viewDirX, point.viewDirY, point.viewDirZ, vx, vy, vz),
      );
      const viewCount = point.independentViews + 1;
      const inv = 1 / viewCount;
      const mx = point.viewDirX + (vx - point.viewDirX) * inv;
      const my = point.viewDirY + (vy - point.viewDirY) * inv;
      const mz = point.viewDirZ + (vz - point.viewDirZ) * inv;
      const length = Math.hypot(mx, my, mz) || 1;
      point.viewDirX = mx / length;
      point.viewDirY = my / length;
      point.viewDirZ = mz / length;
      point.independentViews = Math.min(255, viewCount);
      point.lastEvidenceKeyframe = context.keyframeId;
      point.lastEvidenceEpoch = this.sampleEpoch;

      const cell = this.coverageCells.get(point.coverageKey);
      if (cell !== undefined) this.recordCellEvidence(cell, context);
    }

    if (!point.promoted) {
      const residual = representativeResidual(point);
      if (
        point.independentViews >= PROMOTION_MIN_INDEPENDENT_VIEWS &&
        point.weight >= PROMOTION_MIN_FUSION_WEIGHT &&
        residual <= promotionResidualLimitMeters(depthMeters)
      ) {
        point.promoted = true;
      }
    }
  }

  private findNearbyRepresentative(x: number, y: number, z: number): RepresentativePoint | null {
    const [vx, vy, vz] = voxelCoordinates(x, y, z, this.voxelSizeMeters);
    let best: RepresentativePoint | null = null;
    let bestDistance = Number.POSITIVE_INFINITY;

    for (let dx = -1; dx <= 1; dx += 1) {
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dz = -1; dz <= 1; dz += 1) {
          const point = this.voxelIndex.get(coordinateKey(vx + dx, vy + dy, vz + dz));
          if (point === undefined) continue;
          const distance = Math.hypot(x - point.x, y - point.y, z - point.z);
          if (distance < bestDistance) {
            best = point;
            bestDistance = distance;
          }
        }
      }
    }
    return best;
  }

  private createCoverageCell(): CoverageCell {
    return {
      representatives: new Map<string, RepresentativePoint>(),
      lastEvidenceKeyframe: -1,
      evidenceKeyframes: 0,
      firstCameraX: 0,
      firstCameraY: 0,
      firstCameraZ: 0,
      hasFirstCamera: false,
      maxBaselineMeters: 0,
    };
  }

  private recordCellEvidence(cell: CoverageCell, context: DepthFusionFrameContext): void {
    if (!context.evidenceKeyframe || cell.lastEvidenceKeyframe === context.keyframeId) return;

    cell.lastEvidenceKeyframe = context.keyframeId;
    cell.evidenceKeyframes += 1;
    const camera = context.cameraProjectPosition;
    if (!cell.hasFirstCamera) {
      cell.firstCameraX = camera[0];
      cell.firstCameraY = camera[1];
      cell.firstCameraZ = camera[2];
      cell.hasFirstCamera = true;
      return;
    }

    cell.maxBaselineMeters = Math.max(
      cell.maxBaselineMeters,
      Math.hypot(
        camera[0] - cell.firstCameraX,
        camera[1] - cell.firstCameraY,
        camera[2] - cell.firstCameraZ,
      ),
    );
  }

  private cellQuality(cell: CoverageCell): CellQuality {
    let promotedPoints = 0;
    let qualitySum = 0;
    let maxViewAngle = 0;

    for (const point of cell.representatives.values()) {
      if (!point.promoted) continue;
      promotedPoints += 1;
      qualitySum += representativeQuality(point);
      maxViewAngle = Math.max(maxViewAngle, point.maxViewAngleRadians);
    }

    const averagePointQuality = promotedPoints > 0 ? qualitySum / promotedPoints : 0;
    const densityScore = clamp01(promotedPoints / CELL_READY_MIN_PROMOTED);
    const baselineScore = clamp01(cell.maxBaselineMeters / CELL_TARGET_BASELINE_METERS);
    const viewAngleScore = clamp01(maxViewAngle / ((18 * Math.PI) / 180));
    const geometryDiversity = Math.max(baselineScore, viewAngleScore * 0.75);
    const confidence = clamp01(
      densityScore * 0.45 + geometryDiversity * 0.35 + averagePointQuality * 0.20,
    );
    const visible = promotedPoints > 0 && cell.evidenceKeyframes >= 2;
    const ready =
      promotedPoints >= CELL_READY_MIN_PROMOTED &&
      cell.evidenceKeyframes >= TARGET_INDEPENDENT_VIEWS &&
      cell.maxBaselineMeters >= CELL_READY_BASELINE_METERS &&
      confidence >= 0.70;

    return { confidence, visible, ready, promotedPoints };
  }

  private insertCandidate(cell: CoverageCell, candidate: RepresentativePoint): boolean {
    if (cell.representatives.size < this.adaptiveQuota) {
      if (!this.ensureGlobalSlot(candidate.coverageKey)) return false;
      cell.representatives.set(candidate.voxelKey, candidate);
      this.voxelIndex.set(candidate.voxelKey, candidate);
      this.representativeCount += 1;
      return true;
    }

    const worst = this.findWorstRepresentative(cell);
    if (worst === null || representativeScore(candidate) >= worst.score) return false;

    this.removeRepresentative(cell, worst.point);
    cell.representatives.set(candidate.voxelKey, candidate);
    this.voxelIndex.set(candidate.voxelKey, candidate);
    this.representativeCount += 1;
    this.replacementCount += 1;
    return true;
  }

  private desiredAdaptiveQuota(): number {
    const coverageCount = Math.max(1, this.coverageCells.size);
    return clamp(
      Math.floor(this.memoryBudgetPoints / coverageCount),
      MIN_POINTS_PER_COVERAGE_CELL,
      MAX_POINTS_PER_COVERAGE_CELL,
    );
  }

  private updateAdaptiveQuota(): void {
    const desired = this.desiredAdaptiveQuota();
    if (desired >= this.adaptiveQuota) return;
    this.adaptiveQuota = desired;
    this.trimAllCellsToQuota(desired);
  }

  private trimAllCellsToQuota(quota: number): void {
    for (const cell of this.coverageCells.values()) {
      if (cell.representatives.size <= quota) continue;
      const ranked = [...cell.representatives.values()].sort(
        (left, right) => representativeScore(left) - representativeScore(right),
      );
      for (let index = quota; index < ranked.length; index += 1) {
        this.removeRepresentative(cell, ranked[index]);
        this.rebalanceEvictionCount += 1;
      }
    }
  }

  private ensureGlobalSlot(preferredCoverageKey: string): boolean {
    if (this.representativeCount < this.memoryBudgetPoints) return true;
    this.updateAdaptiveQuota();
    if (this.representativeCount < this.memoryBudgetPoints) return true;

    let donorCell: CoverageCell | null = null;
    let donorWorst: { point: RepresentativePoint; score: number } | null = null;

    for (const [coverageKey, cell] of this.coverageCells) {
      if (coverageKey === preferredCoverageKey || cell.representatives.size <= MIN_POINTS_PER_COVERAGE_CELL) {
        continue;
      }
      const worst = this.findWorstRepresentative(cell);
      if (worst === null) continue;
      if (
        donorCell === null ||
        cell.representatives.size > donorCell.representatives.size ||
        (cell.representatives.size === donorCell.representatives.size &&
          donorWorst !== null && worst.score > donorWorst.score)
      ) {
        donorCell = cell;
        donorWorst = worst;
      }
    }

    if (donorCell === null || donorWorst === null) {
      this.coverageRejectedCount += 1;
      return false;
    }

    this.removeRepresentative(donorCell, donorWorst.point);
    this.rebalanceEvictionCount += 1;
    return true;
  }

  private findWorstRepresentative(
    cell: CoverageCell,
  ): { point: RepresentativePoint; score: number } | null {
    let worst: RepresentativePoint | null = null;
    let worstScore = Number.NEGATIVE_INFINITY;
    for (const point of cell.representatives.values()) {
      const score = representativeScore(point);
      if (score > worstScore) {
        worst = point;
        worstScore = score;
      }
    }
    return worst === null ? null : { point: worst, score: worstScore };
  }

  private removeRepresentative(cell: CoverageCell, point: RepresentativePoint): void {
    if (!cell.representatives.delete(point.voxelKey)) return;
    const indexed = this.voxelIndex.get(point.voxelKey);
    if (indexed === point) this.voxelIndex.delete(point.voxelKey);
    this.representativeCount -= 1;
  }

  private pruneStaleTentative(currentKeyframe: number): void {
    for (const cell of this.coverageCells.values()) {
      const toRemove: RepresentativePoint[] = [];
      for (const point of cell.representatives.values()) {
        if (
          !point.promoted &&
          currentKeyframe - point.lastEvidenceKeyframe > TENTATIVE_MAX_KEYFRAME_AGE
        ) {
          toRemove.push(point);
        }
      }
      for (const point of toRemove) {
        this.removeRepresentative(cell, point);
        this.staleTentativePruned += 1;
      }
    }
  }

  private collectRepresentatives(): RepresentativePoint[] {
    return [...this.voxelIndex.values()];
  }

  private computeFullBounds(positions: readonly number[]): PointCloudBounds | null {
    if (positions.length < 3) return null;
    let minX = Number.POSITIVE_INFINITY;
    let minY = Number.POSITIVE_INFINITY;
    let minZ = Number.POSITIVE_INFINITY;
    let maxX = Number.NEGATIVE_INFINITY;
    let maxY = Number.NEGATIVE_INFINITY;
    let maxZ = Number.NEGATIVE_INFINITY;

    for (let index = 0; index < positions.length; index += 3) {
      const x = positions[index];
      const y = positions[index + 1];
      const z = positions[index + 2];
      minX = Math.min(minX, x); minY = Math.min(minY, y); minZ = Math.min(minZ, z);
      maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); maxZ = Math.max(maxZ, z);
    }
    return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
  }

  private computeRobustBounds(positions: readonly number[]): PointCloudBounds | null {
    const count = positions.length / 3;
    if (count < MIN_POINTS_FOR_ROBUST_BOUNDS) return null;
    const x = sortedAxis(positions, 0);
    const y = sortedAxis(positions, 1);
    const z = sortedAxis(positions, 2);
    const lowIndex = percentileIndex(count, ROBUST_BOUNDS_TRIM_FRACTION);
    const highIndex = percentileIndex(count, 1 - ROBUST_BOUNDS_TRIM_FRACTION);
    return {
      min: [x[lowIndex], y[lowIndex], z[lowIndex]],
      max: [x[highIndex], y[highIndex], z[highIndex]],
    };
  }

  private emptyResult(): DepthFusionResult {
    return {
      changedPoints: 0,
      acceptedSamples: 0,
      createdPoints: 0,
      promotedPoints: 0,
      rejectedResiduals: 0,
    };
  }
}

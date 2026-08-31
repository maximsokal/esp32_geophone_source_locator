export type ProjectPoint = readonly [number, number, number];

export interface PointCloudBounds {
  min: ProjectPoint;
  max: ProjectPoint;
}

export interface ScanPointCloudSnapshot {
  positions: Float32Array;
  pointCount: number;
  /** Retained adaptive representatives before the final stability filter. */
  rawPointCount: number;
  bounds: PointCloudBounds | null;
  fullBounds: PointCloudBounds | null;
  voxelSizeMeters: number;
  maxDepthMeters: number;
  depthFrameCount: number;
  sampledDepthFrames: number;
  /** True only if even one representative per coverage cell could no longer fit. */
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

interface RepresentativePoint {
  x: number;
  y: number;
  z: number;
  observations: number;
  priority: number;
}

interface CoverageCell {
  representatives: Map<string, RepresentativePoint>;
}

const DEFAULT_VOXEL_SIZE_METERS = 0.04;
const DEFAULT_MAX_DEPTH_METERS = 5.0;

// The browser keeps a bounded representative set instead of retaining every
// unique fine voxel forever. 60k representatives are enough for roughly
// 100 m² of 40 mm surface samples while keeping JS Map/object overhead sane on
// mid-range Android devices.
const DEFAULT_MEMORY_BUDGET_POINTS = 60000;
const COVERAGE_CELL_METERS = 0.30;
const MAX_POINTS_PER_COVERAGE_CELL = 28;
const MIN_POINTS_PER_COVERAGE_CELL = 1;

const SAMPLE_COLUMNS = 40;
const SAMPLE_ROWS = 22;
const SAMPLE_MARGIN = 0.025;
const MIN_DEPTH_METERS = 0.18;

const FINAL_SUPPORT_CELL_METERS = 0.12;
const MIN_NEIGHBOUR_SUPPORT = 3;
const MIN_FILTERED_POINTS = 300;
const OBSERVATION_PRIORITY_CAP = 8;

const ROBUST_BOUNDS_TRIM_FRACTION = 0.01;
const MIN_POINTS_FOR_ROBUST_BOUNDS = 1000;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
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

function voxelKey(x: number, y: number, z: number, voxelSizeMeters: number): string {
  return coordinateKey(
    gridCoordinate(x, voxelSizeMeters),
    gridCoordinate(y, voxelSizeMeters),
    gridCoordinate(z, voxelSizeMeters),
  );
}

/** Stable 32-bit hash for deterministic bottom-k spatial sampling. */
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
  const voxelX = gridCoordinate(x, voxelSizeMeters);
  const voxelY = gridCoordinate(y, voxelSizeMeters);
  const voxelZ = gridCoordinate(z, voxelSizeMeters);
  return (hashVoxel(voxelX, voxelY, voxelZ) + 1) / 4294967297;
}

/**
 * Lower is better. Re-observed representatives become harder to evict, while
 * the deterministic hash keeps the retained subset independent of scan order.
 */
function representativeScore(point: RepresentativePoint): number {
  const stabilityWeight = Math.min(point.observations, OBSERVATION_PRIORITY_CAP);
  return point.priority / stabilityWeight;
}

function depthIsLocallyConsistent(
  depth: CpuDepthInformationLike,
  u: number,
  v: number,
  centerDepth: number,
): boolean {
  const deltaU = 2 / Math.max(1, depth.width);
  const deltaV = 2 / Math.max(1, depth.height);
  const toleranceMeters = Math.max(0.10, centerDepth * 0.07);

  const neighbours: readonly [number, number][] = [
    [u - deltaU, v],
    [u + deltaU, v],
    [u, v - deltaV],
    [u, v + deltaV],
  ];

  let agreeing = 0;
  let valid = 0;

  for (const [sampleU, sampleV] of neighbours) {
    if (sampleU <= 0 || sampleU >= 1 || sampleV <= 0 || sampleV >= 1) {
      continue;
    }

    const neighbourDepth = depth.getDepthInMeters(sampleU, sampleV);
    if (!finite(neighbourDepth) || neighbourDepth <= 0) {
      continue;
    }

    valid += 1;
    if (Math.abs(neighbourDepth - centerDepth) <= toleranceMeters) {
      agreeing += 1;
    }
  }

  return valid < 2 || agreeing >= 2;
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
  if (count <= 1) {
    return 0;
  }

  return clamp(Math.round((count - 1) * fraction), 0, count - 1);
}

function supportNeighbourCount(
  support: ReadonlyMap<string, number>,
  x: number,
  y: number,
  z: number,
): number {
  const cellX = gridCoordinate(x, FINAL_SUPPORT_CELL_METERS);
  const cellY = gridCoordinate(y, FINAL_SUPPORT_CELL_METERS);
  const cellZ = gridCoordinate(z, FINAL_SUPPORT_CELL_METERS);

  const offsets: readonly [number, number, number][] = [
    [0, 0, 0],
    [-1, 0, 0],
    [1, 0, 0],
    [0, -1, 0],
    [0, 1, 0],
    [0, 0, -1],
    [0, 0, 1],
  ];

  let count = 0;
  for (const [dx, dy, dz] of offsets) {
    count += support.get(coordinateKey(cellX + dx, cellY + dy, cellZ + dz)) ?? 0;
  }
  return count;
}

/**
 * Bounded-memory metric point-cloud accumulator.
 *
 * Design goals:
 * - 40 mm fine voxels preserve local surface detail.
 * - 300 mm coverage cells divide the global memory budget spatially.
 * - the per-cell quota decreases automatically as new space is discovered.
 * - deterministic bottom-k replacement prevents early viewpoints from owning
 *   the whole budget forever.
 * - repeat observations are averaged and receive a stability bonus.
 * - final support filtering removes isolated representatives without keeping an
 *   unbounded history map during the scan.
 *
 * WebXR uses X-right / Y-up / -Z-forward coordinates. GeoLocator stores:
 *   project X = XR X
 *   project Y = -XR Z
 *   project Z = XR Y (height)
 */
export class DepthPointCloudAccumulator {
  private readonly voxelSizeMeters: number;
  private readonly memoryBudgetPoints: number;
  private maxDepthMeters: number;
  private readonly coverageCells = new Map<string, CoverageCell>();
  private representativeCount = 0;
  private adaptiveQuota = MAX_POINTS_PER_COVERAGE_CELL;
  private candidateSampleCount = 0;
  private discardedSampleCount = 0;
  private replacementCount = 0;
  private rebalanceEvictionCount = 0;
  private coverageRejectedCount = 0;

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
    this.representativeCount = 0;
    this.adaptiveQuota = MAX_POINTS_PER_COVERAGE_CELL;
    this.candidateSampleCount = 0;
    this.discardedSampleCount = 0;
    this.replacementCount = 0;
    this.rebalanceEvictionCount = 0;
    this.coverageRejectedCount = 0;
  }

  setMaxDepthMeters(maxDepthMeters: number): void {
    if (!finite(maxDepthMeters)) {
      return;
    }

    this.maxDepthMeters = clamp(maxDepthMeters, 1.0, 8.0);
  }

  get maxDepth(): number {
    return this.maxDepthMeters;
  }

  /** Number of representatives retained right now, always bounded. */
  get pointCount(): number {
    return this.representativeCount;
  }

  get memoryBudget(): number {
    return this.memoryBudgetPoints;
  }

  get coverageCount(): number {
    return this.coverageCells.size;
  }

  get pointsPerCoverageCell(): number {
    return this.adaptiveQuota;
  }

  get voxelSize(): number {
    return this.voxelSizeMeters;
  }

  sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
  ): number {
    const projection =
      depth.projectionMatrix !== undefined && depth.projectionMatrix.length >= 16
        ? depth.projectionMatrix
        : fallbackView.projectionMatrix;

    const worldFromView =
      depth.transform !== undefined && depth.transform.matrix.length >= 16
        ? depth.transform.matrix
        : fallbackView.transform.matrix;

    if (projection.length < 16 || worldFromView.length < 16) {
      return 0;
    }

    const p00 = Number(projection[0]);
    const p11 = Number(projection[5]);
    const p20 = Number(projection[8]);
    const p21 = Number(projection[9]);

    if (
      !finite(p00) ||
      !finite(p11) ||
      Math.abs(p00) < 1e-6 ||
      Math.abs(p11) < 1e-6
    ) {
      return 0;
    }

    const usableSpan = 1 - SAMPLE_MARGIN * 2;
    let changed = 0;

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

        if (!depthIsLocallyConsistent(depth, u, v, depthMeters)) {
          continue;
        }

        const ndcX = u * 2 - 1;
        const viewX = ((ndcX + p20) * depthMeters) / p00;
        const viewY = ((ndcY + p21) * depthMeters) / p11;
        const viewZ = -depthMeters;

        const worldX =
          Number(worldFromView[0]) * viewX +
          Number(worldFromView[4]) * viewY +
          Number(worldFromView[8]) * viewZ +
          Number(worldFromView[12]);

        const worldY =
          Number(worldFromView[1]) * viewX +
          Number(worldFromView[5]) * viewY +
          Number(worldFromView[9]) * viewZ +
          Number(worldFromView[13]);

        const worldZ =
          Number(worldFromView[2]) * viewX +
          Number(worldFromView[6]) * viewY +
          Number(worldFromView[10]) * viewZ +
          Number(worldFromView[14]);

        if (!finite(worldX) || !finite(worldY) || !finite(worldZ)) {
          continue;
        }

        const projectX = worldX;
        const projectY = -worldZ;
        const projectZ = worldY;
        this.candidateSampleCount += 1;

        if (this.observePoint(projectX, projectY, projectZ)) {
          changed += 1;
        }
      }
    }

    return changed;
  }

  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
    const representatives = this.collectRepresentatives();
    const rawPointCount = representatives.length;
    const support = new Map<string, number>();

    for (const point of representatives) {
      const key = cellKey(point.x, point.y, point.z, FINAL_SUPPORT_CELL_METERS);
      support.set(key, (support.get(key) ?? 0) + 1);
    }

    const stable: RepresentativePoint[] = [];
    for (const point of representatives) {
      if (
        point.observations >= 2 ||
        supportNeighbourCount(support, point.x, point.y, point.z) >= MIN_NEIGHBOUR_SUPPORT
      ) {
        stable.push(point);
      }
    }

    const finalPoints = stable.length >= MIN_FILTERED_POINTS ? stable : representatives;
    const finalPositions = new Array<number>(finalPoints.length * 3);
    let writeIndex = 0;

    for (const point of finalPoints) {
      finalPositions[writeIndex] = point.x;
      finalPositions[writeIndex + 1] = point.y;
      finalPositions[writeIndex + 2] = point.z;
      writeIndex += 3;
    }

    const fullBounds = this.computeFullBounds(finalPositions);
    const bounds = this.computeRobustBounds(finalPositions) ?? fullBounds;

    return {
      positions: Float32Array.from(finalPositions),
      pointCount: finalPoints.length,
      rawPointCount,
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
    };
  }

  private observePoint(x: number, y: number, z: number): boolean {
    const coverageKey = cellKey(x, y, z, COVERAGE_CELL_METERS);
    let cell = this.coverageCells.get(coverageKey);

    if (cell === undefined) {
      if (this.coverageCells.size >= this.memoryBudgetPoints) {
        // One representative per coverage cell is the absolute lower bound.
        this.coverageRejectedCount += 1;
        this.discardedSampleCount += 1;
        return false;
      }

      cell = { representatives: new Map<string, RepresentativePoint>() };
      this.coverageCells.set(coverageKey, cell);
      this.updateAdaptiveQuota();
    }

    const fineKey = voxelKey(x, y, z, this.voxelSizeMeters);
    const existing = cell.representatives.get(fineKey);

    if (existing !== undefined) {
      const nextObservations = existing.observations + 1;
      const alpha = 1 / nextObservations;
      existing.x += (x - existing.x) * alpha;
      existing.y += (y - existing.y) * alpha;
      existing.z += (z - existing.z) * alpha;
      existing.observations = nextObservations;
      return true;
    }

    const candidate: RepresentativePoint = {
      x,
      y,
      z,
      observations: 1,
      priority: voxelPriority(x, y, z, this.voxelSizeMeters),
    };

    if (cell.representatives.size < this.adaptiveQuota) {
      if (!this.ensureGlobalSlot(coverageKey)) {
        this.discardedSampleCount += 1;
        return false;
      }

      cell.representatives.set(fineKey, candidate);
      this.representativeCount += 1;
      return true;
    }

    const worst = this.findWorstRepresentative(cell);
    if (worst === null || representativeScore(candidate) >= worst.score) {
      this.discardedSampleCount += 1;
      return false;
    }

    cell.representatives.delete(worst.key);
    cell.representatives.set(fineKey, candidate);
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
    if (desired >= this.adaptiveQuota) {
      return;
    }

    this.adaptiveQuota = desired;
    this.trimAllCellsToQuota(desired);
  }

  private trimAllCellsToQuota(quota: number): void {
    for (const cell of this.coverageCells.values()) {
      if (cell.representatives.size <= quota) {
        continue;
      }

      const ranked = [...cell.representatives.entries()].sort(
        (left, right) => representativeScore(left[1]) - representativeScore(right[1]),
      );

      for (let index = quota; index < ranked.length; index += 1) {
        cell.representatives.delete(ranked[index][0]);
        this.representativeCount -= 1;
        this.rebalanceEvictionCount += 1;
      }
    }
  }

  private ensureGlobalSlot(preferredCoverageKey: string): boolean {
    if (this.representativeCount < this.memoryBudgetPoints) {
      return true;
    }

    this.updateAdaptiveQuota();
    if (this.representativeCount < this.memoryBudgetPoints) {
      return true;
    }

    // A newly discovered cell should still be able to claim coverage. Reclaim
    // one low-quality representative from an over-populated cell rather than
    // freezing the map at whichever viewpoint filled the budget first.
    let donorCell: CoverageCell | null = null;
    let donorKey: string | null = null;
    let donorWorst: { key: string; score: number } | null = null;

    for (const [coverageKey, cell] of this.coverageCells) {
      if (coverageKey === preferredCoverageKey || cell.representatives.size <= MIN_POINTS_PER_COVERAGE_CELL) {
        continue;
      }

      const worst = this.findWorstRepresentative(cell);
      if (worst === null) {
        continue;
      }

      if (
        donorCell === null ||
        cell.representatives.size > donorCell.representatives.size ||
        (cell.representatives.size === donorCell.representatives.size &&
          donorWorst !== null && worst.score > donorWorst.score)
      ) {
        donorCell = cell;
        donorKey = coverageKey;
        donorWorst = worst;
      }
    }

    if (donorCell === null || donorKey === null || donorWorst === null) {
      this.coverageRejectedCount += 1;
      return false;
    }

    donorCell.representatives.delete(donorWorst.key);
    this.representativeCount -= 1;
    this.rebalanceEvictionCount += 1;
    return true;
  }

  private findWorstRepresentative(
    cell: CoverageCell,
  ): { key: string; score: number } | null {
    let worstKey: string | null = null;
    let worstScore = Number.NEGATIVE_INFINITY;

    for (const [key, point] of cell.representatives) {
      const score = representativeScore(point);
      if (score > worstScore) {
        worstKey = key;
        worstScore = score;
      }
    }

    return worstKey === null ? null : { key: worstKey, score: worstScore };
  }

  private collectRepresentatives(): RepresentativePoint[] {
    const points = new Array<RepresentativePoint>(this.representativeCount);
    let writeIndex = 0;

    for (const cell of this.coverageCells.values()) {
      for (const point of cell.representatives.values()) {
        points[writeIndex] = point;
        writeIndex += 1;
      }
    }

    if (writeIndex !== points.length) {
      points.length = writeIndex;
    }
    return points;
  }

  private computeFullBounds(positions: readonly number[]): PointCloudBounds | null {
    if (positions.length < 3) {
      return null;
    }

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

      minX = Math.min(minX, x);
      minY = Math.min(minY, y);
      minZ = Math.min(minZ, z);
      maxX = Math.max(maxX, x);
      maxY = Math.max(maxY, y);
      maxZ = Math.max(maxZ, z);
    }

    return {
      min: [minX, minY, minZ],
      max: [maxX, maxY, maxZ],
    };
  }

  private computeRobustBounds(positions: readonly number[]): PointCloudBounds | null {
    const count = positions.length / 3;
    if (count < MIN_POINTS_FOR_ROBUST_BOUNDS) {
      return null;
    }

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
}

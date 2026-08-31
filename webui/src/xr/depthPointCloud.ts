export type ProjectPoint = readonly [number, number, number];

export interface PointCloudBounds {
  min: ProjectPoint;
  max: ProjectPoint;
}

export interface ScanPointCloudSnapshot {
  positions: Float32Array;
  pointCount: number;
  rawPointCount: number;
  bounds: PointCloudBounds | null;
  voxelSizeMeters: number;
  maxDepthMeters: number;
  depthFrameCount: number;
  sampledDepthFrames: number;
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

const DEFAULT_VOXEL_SIZE_METERS = 0.04;
const DEFAULT_MAX_POINTS = 100000;
const DEFAULT_MAX_DEPTH_METERS = 5.0;
const SAMPLE_COLUMNS = 40;
const SAMPLE_ROWS = 22;
const SAMPLE_MARGIN = 0.025;
const MIN_DEPTH_METERS = 0.18;
const SUPPORT_CELL_METERS = 0.12;
const MIN_SUPPORT_HITS = 2;
const MIN_FILTERED_POINTS = 300;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function voxelCoordinate(value: number, voxelSizeMeters: number): number {
  return Math.floor(value / voxelSizeMeters);
}

function cellKey(x: number, y: number, z: number, cellSizeMeters: number): string {
  return [
    voxelCoordinate(x, cellSizeMeters),
    voxelCoordinate(y, cellSizeMeters),
    voxelCoordinate(z, cellSizeMeters),
  ].join(':');
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

  // Keep silhouettes/edges when there are not enough valid neighbours,
  // otherwise require at least two nearby pixels to agree with the sample.
  return valid < 2 || agreeing >= 2;
}

/**
 * Metric point-cloud accumulator for browser-only AR scanning.
 *
 * WebXR uses X-right / Y-up / -Z-forward coordinates. GeoLocator stores:
 *   project X = XR X
 *   project Y = -XR Z
 *   project Z = XR Y (height)
 *
 * The live cloud is voxel-decimated at 40 mm. A coarser 120 mm support grid
 * counts repeated observations so isolated one-frame depth points can be
 * removed when the scan is finalized.
 */
export class DepthPointCloudAccumulator {
  private readonly voxelSizeMeters: number;
  private readonly maxPoints: number;
  private maxDepthMeters: number;
  private readonly occupiedVoxels = new Set<string>();
  private readonly supportCells = new Map<string, number>();
  private readonly positions: number[] = [];

  constructor(
    voxelSizeMeters = DEFAULT_VOXEL_SIZE_METERS,
    maxPoints = DEFAULT_MAX_POINTS,
    maxDepthMeters = DEFAULT_MAX_DEPTH_METERS,
  ) {
    this.voxelSizeMeters = voxelSizeMeters;
    this.maxPoints = maxPoints;
    this.maxDepthMeters = maxDepthMeters;
  }

  reset(): void {
    this.occupiedVoxels.clear();
    this.supportCells.clear();
    this.positions.length = 0;
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

  get pointCount(): number {
    return this.positions.length / 3;
  }

  get voxelSize(): number {
    return this.voxelSizeMeters;
  }

  sampleDepthFrame(
    depth: CpuDepthInformationLike,
    fallbackView: DepthSampleView,
  ): number {
    if (this.pointCount >= this.maxPoints) {
      return 0;
    }

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
    let added = 0;

    for (let row = 0; row < SAMPLE_ROWS; row += 1) {
      const v = SAMPLE_MARGIN + ((row + 0.5) / SAMPLE_ROWS) * usableSpan;
      const ndcY = 1 - v * 2;

      for (let column = 0; column < SAMPLE_COLUMNS; column += 1) {
        if (this.pointCount >= this.maxPoints) {
          return added;
        }

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

        // WebXR depth is perpendicular distance from the camera/sensor plane,
        // not Euclidean ray length. Therefore view-space Z is -depth.
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

        const supportKey = cellKey(
          projectX,
          projectY,
          projectZ,
          SUPPORT_CELL_METERS,
        );
        this.supportCells.set(supportKey, (this.supportCells.get(supportKey) ?? 0) + 1);

        if (this.addPoint(projectX, projectY, projectZ)) {
          added += 1;
        }
      }
    }

    return added;
  }

  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
    const rawPointCount = this.pointCount;
    const supported: number[] = [];

    for (let index = 0; index < this.positions.length; index += 3) {
      const x = this.positions[index];
      const y = this.positions[index + 1];
      const z = this.positions[index + 2];
      const support = this.supportCells.get(cellKey(x, y, z, SUPPORT_CELL_METERS)) ?? 0;

      if (support >= MIN_SUPPORT_HITS) {
        supported.push(x, y, z);
      }
    }

    const finalPositions =
      supported.length / 3 >= MIN_FILTERED_POINTS
        ? supported
        : this.positions;

    const bounds = this.computeBounds(finalPositions);

    return {
      positions: Float32Array.from(finalPositions),
      pointCount: finalPositions.length / 3,
      rawPointCount,
      bounds,
      voxelSizeMeters: this.voxelSizeMeters,
      maxDepthMeters: this.maxDepthMeters,
      depthFrameCount,
      sampledDepthFrames,
    };
  }

  private computeBounds(positions: readonly number[]): PointCloudBounds | null {
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

  private addPoint(x: number, y: number, z: number): boolean {
    const key = cellKey(x, y, z, this.voxelSizeMeters);

    if (this.occupiedVoxels.has(key)) {
      return false;
    }

    this.occupiedVoxels.add(key);
    this.positions.push(x, y, z);
    return true;
  }
}

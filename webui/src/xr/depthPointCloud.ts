export type ProjectPoint = readonly [number, number, number];

export interface PointCloudBounds {
  min: ProjectPoint;
  max: ProjectPoint;
}

export interface ScanPointCloudSnapshot {
  positions: Float32Array;
  pointCount: number;
  bounds: PointCloudBounds | null;
  voxelSizeMeters: number;
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
}

const DEFAULT_VOXEL_SIZE_METERS = 0.04;
const DEFAULT_MAX_POINTS = 75000;
const SAMPLE_COLUMNS = 18;
const SAMPLE_ROWS = 14;
const SAMPLE_MARGIN = 0.035;
const MIN_DEPTH_METERS = 0.18;
const MAX_DEPTH_METERS = 8.0;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function voxelCoordinate(value: number, voxelSizeMeters: number): number {
  return Math.floor(value / voxelSizeMeters);
}

/**
 * Sparse metric point cloud accumulator for browser-only AR scanning.
 *
 * WebXR uses X-right / Y-up / -Z-forward coordinates. GeoLocator uses
 * X/Y horizontal and Z height, so accepted points are stored as:
 *   project X = XR X
 *   project Y = -XR Z
 *   project Z = XR Y
 */
export class DepthPointCloudAccumulator {
  private readonly voxelSizeMeters: number;
  private readonly maxPoints: number;
  private readonly occupiedVoxels = new Set<string>();
  private readonly positions: number[] = [];
  private minX = Number.POSITIVE_INFINITY;
  private minY = Number.POSITIVE_INFINITY;
  private minZ = Number.POSITIVE_INFINITY;
  private maxX = Number.NEGATIVE_INFINITY;
  private maxY = Number.NEGATIVE_INFINITY;
  private maxZ = Number.NEGATIVE_INFINITY;

  constructor(
    voxelSizeMeters = DEFAULT_VOXEL_SIZE_METERS,
    maxPoints = DEFAULT_MAX_POINTS,
  ) {
    this.voxelSizeMeters = voxelSizeMeters;
    this.maxPoints = maxPoints;
  }

  reset(): void {
    this.occupiedVoxels.clear();
    this.positions.length = 0;
    this.minX = Number.POSITIVE_INFINITY;
    this.minY = Number.POSITIVE_INFINITY;
    this.minZ = Number.POSITIVE_INFINITY;
    this.maxX = Number.NEGATIVE_INFINITY;
    this.maxY = Number.NEGATIVE_INFINITY;
    this.maxZ = Number.NEGATIVE_INFINITY;
  }

  get pointCount(): number {
    return this.positions.length / 3;
  }

  get voxelSize(): number {
    return this.voxelSizeMeters;
  }

  sampleDepthFrame(
    depth: CpuDepthInformationLike,
    view: DepthSampleView,
  ): number {
    if (this.pointCount >= this.maxPoints) {
      return 0;
    }

    const projection = view.projectionMatrix;
    const worldFromView = view.transform.matrix;

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
          depthMeters > MAX_DEPTH_METERS
        ) {
          continue;
        }

        const ndcX = u * 2 - 1;

        // WebXR depth is the perpendicular distance from the camera plane,
        // not Euclidean ray length. Reconstruct a view-space point from the
        // XR projection matrix using z = -depth.
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

        // Convert WebXR world coordinates to the diploma/project convention:
        // X/Y horizontal, Z height.
        const projectX = worldX;
        const projectY = -worldZ;
        const projectZ = worldY;

        if (this.addPoint(projectX, projectY, projectZ)) {
          added += 1;
        }
      }
    }

    return added;
  }

  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
    const pointCount = this.pointCount;

    return {
      positions: Float32Array.from(this.positions),
      pointCount,
      bounds:
        pointCount === 0
          ? null
          : {
              min: [this.minX, this.minY, this.minZ],
              max: [this.maxX, this.maxY, this.maxZ],
            },
      voxelSizeMeters: this.voxelSizeMeters,
      depthFrameCount,
      sampledDepthFrames,
    };
  }

  private addPoint(x: number, y: number, z: number): boolean {
    const voxelX = voxelCoordinate(x, this.voxelSizeMeters);
    const voxelY = voxelCoordinate(y, this.voxelSizeMeters);
    const voxelZ = voxelCoordinate(z, this.voxelSizeMeters);
    const key = `${voxelX}:${voxelY}:${voxelZ}`;

    if (this.occupiedVoxels.has(key)) {
      return false;
    }

    this.occupiedVoxels.add(key);
    this.positions.push(x, y, z);

    this.minX = Math.min(this.minX, x);
    this.minY = Math.min(this.minY, y);
    this.minZ = Math.min(this.minZ, z);
    this.maxX = Math.max(this.maxX, x);
    this.maxY = Math.max(this.maxY, y);
    this.maxZ = Math.max(this.maxZ, z);

    return true;
  }
}

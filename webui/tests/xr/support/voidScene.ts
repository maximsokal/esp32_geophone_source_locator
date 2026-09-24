import {
  encodeOccupancySolverArtifact,
  type OccupancyRegistration,
} from '../../../src/xr/occupancySolverArtifact';

/*
 * Shared builder for the void-scene occupancy artifact (see
 * ../occupancyVoidSceneVector.test.ts for the scene and its purpose).
 */
export const VOXEL = 0.1;
export const GRID = 30;
export const TRENCH = { lowIndex: 14, highIndex: 16, bottomIndex: -9 };

export const VOID_SCENE_REGISTRATION: OccupancyRegistration = {
  source: 'apriltag',
  geometry_computed_at_ms: 424242,
  sensor_to_scan: [0, -1, 0, 10, 1, 0, 0, -5, 0, 0, 1, 2, 0, 0, 0, 1],
  residual_m: 0.004,
};

export function toScan(x: number, y: number, z: number): [number, number, number] {
  const m = VOID_SCENE_REGISTRATION.sensor_to_scan;
  return [
    m[0] * x + m[1] * y + m[2] * z + m[3],
    m[4] * x + m[5] * y + m[6] * z + m[7],
    m[8] * x + m[9] * y + m[10] * z + m[11],
  ];
}

function center(index: number): number {
  return (index + 0.5) * VOXEL;
}

function inTrench(ix: number): boolean {
  return ix >= TRENCH.lowIndex && ix <= TRENCH.highIndex;
}

function voidSceneSnapshot() {
  const free: number[] = [];
  const occupied: number[] = [];
  for (let ix = 0; ix < GRID; ix += 1) {
    for (let iy = 0; iy < GRID; iy += 1) {
      for (const iz of [0, 1]) free.push(...toScan(center(ix), center(iy), center(iz)));
      if (inTrench(ix)) {
        for (let iz = TRENCH.bottomIndex; iz <= -1; iz += 1) free.push(...toScan(center(ix), center(iy), center(iz)));
      } else {
        occupied.push(...toScan(center(ix), center(iy), center(-1)));
      }
    }
  }
  const corners = [
    toScan(0, 0, -0.1), toScan(3, 3, 0.2), toScan(0, 3, -0.1), toScan(3, 0, 0.2),
  ];
  const min = [0, 1, 2].map((axis) => Math.min(...corners.map((corner) => corner[axis]))) as [number, number, number];
  const max = [0, 1, 2].map((axis) => Math.max(...corners.map((corner) => corner[axis]))) as [number, number, number];
  return {
    freeAirPositions: new Float32Array(free),
    occupiedSurfacePositions: new Float32Array(occupied),
    occupancyEnvelopeBounds: { min, max },
    occupancySolverCandidateBelowSurfaceMeters: 2,
    occupancyCellsSaturated: false,
    occupancyFineVoxelSizeMeters: VOXEL,
    occupancyVoxelSizeMeters: VOXEL,
  };
}

const OPTIONS = {
  frameId: 'scan-crc32:0badc0de',
  createdAtMs: 1700000000000,
  registration: VOID_SCENE_REGISTRATION,
};

export function encodeVoidScene(): Uint8Array {
  return encodeOccupancySolverArtifact(voidSceneSnapshot(), OPTIONS);
}


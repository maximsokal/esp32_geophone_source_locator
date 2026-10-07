export type UncertaintyVec3 = readonly [number, number, number];

export interface ProjectAxisAlignedBox {
  min: UncertaintyVec3;
  max: UncertaintyVec3;
}

function usableMatrix(matrix: readonly number[]): boolean {
  return matrix.length === 16 && matrix.every(Number.isFinite);
}

/** Sensor-frame independent one-sigma extents projected onto project axes. */
export function sensorSigmasToProjectHalfExtents(
  sensorSigmas: UncertaintyVec3,
  sensorToProject: readonly number[],
): UncertaintyVec3 | null {
  if (!usableMatrix(sensorToProject) || !sensorSigmas.every((sigma) => Number.isFinite(sigma) && sigma >= 0)) return null;
  const extents = [0, 1, 2].map((projectAxis) => Math.sqrt(
    sensorSigmas.reduce((sum, sigma, sensorAxis) => {
      const projected = sensorToProject[projectAxis * 4 + sensorAxis] * sigma;
      return sum + projected * projected;
    }, 0),
  ));
  return extents.every(Number.isFinite) ? extents as unknown as UncertaintyVec3 : null;
}

/** Transforms all eight corners of a sensor-frame box and returns its project-frame AABB. */
export function transformSensorBoxToProjectAabb(
  sensorMin: UncertaintyVec3,
  sensorMax: UncertaintyVec3,
  sensorToProject: readonly number[],
): ProjectAxisAlignedBox | null {
  if (!usableMatrix(sensorToProject) || !sensorMin.every(Number.isFinite) || !sensorMax.every(Number.isFinite) ||
      sensorMin.some((value, axis) => value > sensorMax[axis])) return null;
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const x of [sensorMin[0], sensorMax[0]]) {
    for (const y of [sensorMin[1], sensorMax[1]]) {
      for (const z of [sensorMin[2], sensorMax[2]]) {
        const sensor = [x, y, z];
        const project = [0, 1, 2].map((axis) =>
          sensorToProject[axis * 4] * sensor[0] +
          sensorToProject[axis * 4 + 1] * sensor[1] +
          sensorToProject[axis * 4 + 2] * sensor[2] +
          sensorToProject[axis * 4 + 3],
        );
        if (!project.every(Number.isFinite)) return null;
        for (let axis = 0; axis < 3; axis += 1) {
          min[axis] = Math.min(min[axis], project[axis]);
          max[axis] = Math.max(max[axis], project[axis]);
        }
      }
    }
  }
  return { min: min as unknown as UncertaintyVec3, max: max as unknown as UncertaintyVec3 };
}

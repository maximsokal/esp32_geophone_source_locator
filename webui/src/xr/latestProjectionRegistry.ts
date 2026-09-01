import {
  normalizedScreenRayToProject,
  type ProjectRay,
} from './manualRayEdgePlacement';

let projection: Float32Array | null = null;
let worldFromView: Float32Array | null = null;

export function setLatestProjectionMatrices(
  projectionMatrix: ArrayLike<number>,
  worldFromViewMatrix: ArrayLike<number>,
): void {
  if (projectionMatrix.length < 16 || worldFromViewMatrix.length < 16) return;
  projection = Float32Array.from(Array.from({ length: 16 }, (_, index) => Number(projectionMatrix[index])));
  worldFromView = Float32Array.from(Array.from({ length: 16 }, (_, index) => Number(worldFromViewMatrix[index])));
}

export function getLatestProjectRay(u: number, v: number): ProjectRay | null {
  return projection === null || worldFromView === null
    ? null
    : normalizedScreenRayToProject(u, v, projection, worldFromView);
}

export function resetLatestProjectionMatrices(): void {
  projection = null;
  worldFromView = null;
}

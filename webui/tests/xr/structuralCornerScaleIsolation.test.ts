import { describe, expect, it } from 'vitest';

import {
  detectStructuralGuideGeometry,
  type ProjectPoint,
  type StructuralGuideInput,
} from '../../src/xr/structuralGuide';

interface Cloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): Cloud {
  return { positions: [], normals: [], confidence: [] };
}

function addPoint(target: Cloud, position: ProjectPoint, normal: ProjectPoint): void {
  target.positions.push(...position);
  target.normals.push(...normal);
  target.confidence.push(0.95);
}

function addPlaneZ(
  target: Cloud,
  z: number,
  minX: number,
  maxX: number,
  minY: number,
  maxY: number,
  step: number,
): void {
  for (let x = minX; x <= maxX + 1e-7; x += step) {
    for (let y = minY; y <= maxY + 1e-7; y += step) addPoint(target, [x, y, z], [0, 0, 1]);
  }
}

function addPlaneX(
  target: Cloud,
  x: number,
  minY: number,
  maxY: number,
  minZ: number,
  maxZ: number,
  step: number,
): void {
  for (let y = minY; y <= maxY + 1e-7; y += step) {
    for (let z = minZ; z <= maxZ + 1e-7; z += step) addPoint(target, [x, y, z], [1, 0, 0]);
  }
}

function addPlaneY(
  target: Cloud,
  y: number,
  minX: number,
  maxX: number,
  minZ: number,
  maxZ: number,
  step: number,
): void {
  for (let x = minX; x <= maxX + 1e-7; x += step) {
    for (let z = minZ; z <= maxZ + 1e-7; z += step) addPoint(target, [x, y, z], [0, 1, 0]);
  }
}

function inputFrom(target: Cloud): StructuralGuideInput {
  return {
    positions: Float32Array.from(target.positions),
    normals: Float32Array.from(target.normals),
    confidence: Float32Array.from(target.confidence),
    keyframeCount: 12,
    voxelSizeMeters: 0.04,
    viewpoint: { position: [0.45, 0.45, 0.45], keyframeId: 12 },
  };
}

describe('corner-specific physical scale isolation', () => {
  it('does not let a tiny three-plane speck borrow the scale of a large connected component', () => {
    const target = cloud();

    // This mature measured slab makes the connected physical component large.
    // It deliberately passes through the origin so the two tiny orthogonal
    // patches below belong to the same spatial component rather than being
    // rejected merely as disconnected noise.
    addPlaneZ(target, 0, -0.24, 0.24, -0.24, 0.24, 0.03);

    // Dense but physically tiny x/y patches create an algebraically excellent
    // three-plane solution at the origin. Their local arms are only about 4 cm,
    // below the corner-specific finite-support scale policy. Global component
    // maturity must never turn this chip into an operational corner.
    addPlaneX(target, 0, -0.02, 0.02, 0, 0.04, 0.01);
    addPlaneY(target, 0, -0.02, 0.02, 0, 0.04, 0.01);

    const result = detectStructuralGuideGeometry(inputFrom(target));

    expect(Math.floor(target.positions.length / 3)).toBeGreaterThan(250);
    expect(result.planarPatches).toBeGreaterThanOrEqual(3);
    expect(result.corners).toHaveLength(0);
  });
});

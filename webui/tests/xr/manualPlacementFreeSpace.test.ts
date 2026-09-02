import { describe, expect, it } from 'vitest';

import {
  solveManualPlacementOnStableSurface,
  type ManualPlacementSurfaceSnapshot,
} from '../../src/xr/manualPlacementSurface';

interface MutableCloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function cloud(): MutableCloud { return { positions: [], normals: [], confidence: [] }; }

function add(
  target: MutableCloud,
  position: readonly [number, number, number],
  normal: readonly [number, number, number],
): void {
  target.positions.push(...position);
  target.normals.push(...normal);
  target.confidence.push(0.92);
}

function addPlaneX(target: MutableCloud): void {
  for (const y of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(target, [0, y, z], [1, 0, 0]);
  }
}

function addPlaneY(target: MutableCloud): void {
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.06, -0.02, 0.02, 0.06]) add(target, [x, 0, z], [0, 1, 0]);
  }
}

function addPlaneZ(target: MutableCloud): void {
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const y of [-0.06, -0.02, 0.02, 0.06]) add(target, [x, y, 0], [0, 0, 1]);
  }
}

function snapshot(source: MutableCloud, free = false): ManualPlacementSurfaceSnapshot {
  return {
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    pointCount: source.confidence.length,
    voxelSizeMeters: 0.04,
    ...(free ? {
      freeSpacePositions: Float32Array.from([0.001, 0.001, 0.001]),
      freeSpaceVoxelSizeMeters: 0.04,
    } : {}),
  };
}

describe('manual placement verified FREE veto', () => {
  it('accepts a supported robust edge when its solved voxel is not verified FREE', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    const result = solveManualPlacementOnStableSurface([0.025, -0.018, 0.02], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('edge');
  });

  it('rejects the same robust edge when its solved position lands in verified FREE', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    const result = solveManualPlacementOnStableSurface([0.025, -0.018, 0.02], snapshot(source, true));
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
    expect(result.message).toContain('FREE');
  });

  it('accepts a supported robust three-plane corner without a FREE conflict', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    addPlaneZ(source);
    const result = solveManualPlacementOnStableSurface([0.022, -0.018, 0.027], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('corner');
  });

  it('rejects the same robust corner when the solved vertex is verified FREE', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    addPlaneZ(source);
    const result = solveManualPlacementOnStableSurface([0.022, -0.018, 0.027], snapshot(source, true));
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
    expect(result.message).toContain('FREE');
  });
});

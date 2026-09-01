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

function cloud(): MutableCloud {
  return { positions: [], normals: [], confidence: [] };
}

function add(
  target: MutableCloud,
  position: readonly [number, number, number],
  normal: readonly [number, number, number],
  confidence = 0.92,
): void {
  target.positions.push(...position);
  target.normals.push(...normal);
  target.confidence.push(confidence);
}

function snapshot(source: MutableCloud): ManualPlacementSurfaceSnapshot {
  return {
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    pointCount: source.confidence.length,
    voxelSizeMeters: 0.04,
  };
}

function addPlaneX(target: MutableCloud): void {
  for (const y of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.04, 0, 0.04]) add(target, [0, y, z], [1, 0, 0]);
  }
}

function addPlaneY(target: MutableCloud): void {
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.04, 0, 0.04]) add(target, [x, 0, z], [0, 1, 0]);
  }
}

function addPlaneZ(target: MutableCloud): void {
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const y of [-0.04, 0, 0.04]) add(target, [x, y, 0], [0, 0, 1]);
  }
}

describe('manual placement on accumulated surface geometry', () => {
  it('locks a cursor depth candidate to a reconstructed two-plane edge', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);

    const result = solveManualPlacementOnStableSurface([0.025, -0.018, 0.02], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('edge');
    expect(result.position).not.toBeNull();
    expect(result.position![0]).toBeCloseTo(0, 3);
    expect(result.position![1]).toBeCloseTo(0, 3);
    expect(result.position![2]).toBeCloseTo(0.02, 2);
    expect(result.supportCount).toBeGreaterThanOrEqual(4);
  });

  it('solves a three-plane structural corner instead of keeping raw depth', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    addPlaneZ(source);

    const result = solveManualPlacementOnStableSurface([0.022, -0.018, 0.027], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('corner');
    expect(result.position).not.toBeNull();
    expect(result.position![0]).toBeCloseTo(0, 3);
    expect(result.position![1]).toBeCloseTo(0, 3);
    expect(result.position![2]).toBeCloseTo(0, 3);
  });

  it('rejects a tap when only one surface family has been accumulated', () => {
    const source = cloud();
    addPlaneX(source);

    const result = solveManualPlacementOnStableSurface([0.01, 0.01, 0], snapshot(source));
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
    expect(result.message).toContain('another side');
  });

  it('rejects a flying-depth candidate far away from all promoted surface support', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);

    const result = solveManualPlacementOnStableSurface([0.45, 0.45, 0], snapshot(source));
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
    expect(result.message).toContain('does not agree');
  });
});

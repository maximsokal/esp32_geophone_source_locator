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

function addTinyPlane(target: MutableCloud, axis: 0 | 1 | 2): void {
  const offsets = [-0.035, -0.012, 0.012, 0.035];
  for (const first of offsets) {
    for (const second of offsets) {
      const position: [number, number, number] = [0, 0, 0];
      const normal: [number, number, number] = [0, 0, 0];
      normal[axis] = 1;
      const tangentAxes = ([0, 1, 2] as const).filter((candidate) => candidate !== axis);
      position[tangentAxes[0]] = first;
      position[tangentAxes[1]] = second;
      add(target, position, normal);
    }
  }
}

function addPlaneWithCornerHole(
  target: MutableCloud,
  axis: 0 | 1 | 2,
): void {
  const offsets = [-0.085, -0.055, 0.055, 0.085];
  for (const first of offsets) {
    for (const second of offsets) {
      const position: [number, number, number] = [0, 0, 0];
      const normal: [number, number, number] = [0, 0, 0];
      normal[axis] = 1;
      const tangentAxes = ([0, 1, 2] as const).filter((candidate) => candidate !== axis);
      position[tangentAxes[0]] = first;
      position[tangentAxes[1]] = second;
      add(target, position, normal);
    }
  }
}

describe('manual placement on accumulated surface geometry', () => {
  it('locks a cursor candidate to a reconstructed robust two-plane edge', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);

    const result = solveManualPlacementOnStableSurface([0.025, -0.018, 0.02], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('edge');
    expect(result.position).not.toBeNull();
    expect(result.position![0]).toBeCloseTo(0, 2);
    expect(result.position![1]).toBeCloseTo(0, 2);
    expect(result.position![2]).toBeCloseTo(0.02, 1);
    expect(result.supportCount).toBeGreaterThanOrEqual(12);
    expect(result.uncertaintyMeters).toBeLessThan(0.035);
    expect(result.confidence).toBeLessThan(1);
  });

  it('solves a conditioned three-plane structural corner instead of keeping seed depth', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    addPlaneZ(source);

    const result = solveManualPlacementOnStableSurface([0.022, -0.018, 0.027], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('corner');
    expect(result.position).not.toBeNull();
    expect(result.position![0]).toBeCloseTo(0, 2);
    expect(result.position![1]).toBeCloseTo(0, 2);
    expect(result.position![2]).toBeCloseTo(0, 2);
    expect(result.uncertaintyMeters).toBeLessThan(0.032);
    expect(result.conditionNumber).toBeLessThan(35);
    expect(result.minimumCornerArmSpanMeters).toBeGreaterThanOrEqual(0.055);
    expect(result.confidence).toBeLessThan(1);
  });

  it('does not promote three tiny but mathematically clean patches to a structural corner', () => {
    const source = cloud();
    addTinyPlane(source, 0);
    addTinyPlane(source, 1);
    addTinyPlane(source, 2);

    const result = solveManualPlacementOnStableSurface([0.005, -0.005, 0.004], snapshot(source));
    expect(result.mode).not.toBe('corner');
    expect(result.minimumCornerArmSpanMeters ?? 0).toBeLessThan(0.055);
  });

  it('infers a three-plane corner even when consumer depth has no sample at the physical vertex', () => {
    const source = cloud();
    addPlaneWithCornerHole(source, 0);
    addPlaneWithCornerHole(source, 1);
    addPlaneWithCornerHole(source, 2);

    // The nearest measured point is about 7.8 cm from the true vertex. The
    // corner must come from the three robust plane equations, not from a density
    // peak or a depth texel exactly at [0,0,0].
    const result = solveManualPlacementOnStableSurface([0.012, -0.010, 0.008], snapshot(source));
    expect(result.accepted).toBe(true);
    expect(result.mode).toBe('corner');
    expect(result.position).not.toBeNull();
    expect(result.position![0]).toBeCloseTo(0, 2);
    expect(result.position![1]).toBeCloseTo(0, 2);
    expect(result.position![2]).toBeCloseTo(0, 2);
    expect(result.sourceDistanceMeters).toBeGreaterThan(0.055);
    expect(result.minimumCornerArmSpanMeters).toBeGreaterThan(0.055);
    expect(result.uncertaintyMeters).toBeLessThan(0.032);
  });

  it('rejects a tap when only one robust surface has been accumulated', () => {
    const source = cloud();
    addPlaneX(source);

    const result = solveManualPlacementOnStableSurface([0.01, 0.01, 0], snapshot(source));
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
    expect(result.message).toContain('other side');
  });

  it('rejects a flying-depth candidate far away from all promoted surface support', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);

    const result = solveManualPlacementOnStableSurface([0.45, 0.45, 0], snapshot(source));
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
    expect(result.message).toContain('not supported');
  });

  it('does not call a few normal-consistent points a plane', () => {
    const source = cloud();
    addPlaneX(source);
    for (let index = 0; index < 3; index += 1) {
      add(source, [0.03 + index * 0.01, 0.01, 0.01], [0, 1, 0], 0.95);
    }
    const result = solveManualPlacementOnStableSurface([0.01, 0.01, 0.01], snapshot(source));
    expect(result.accepted).toBe(false);
    expect(result.mode).toBeNull();
  });

  it('rejects high-residual evidence through the optional uncertainty certificate', () => {
    const source = cloud();
    addPlaneX(source);
    addPlaneY(source);
    const base = snapshot(source);
    const residualMeters = new Float32Array(base.pointCount).fill(0.055);
    const result = solveManualPlacementOnStableSurface([0.01, 0.01, 0], {
      ...base,
      residualMeters,
    });
    expect(result.accepted).toBe(false);
    expect(result.position).toBeNull();
  });
});
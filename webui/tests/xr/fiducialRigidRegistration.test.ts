import { describe, expect, it } from 'vitest';
import { registerSensorToScan, svd3, type FusedRegistrationTag, type RegistrationPodGeometry, type Vector3 } from '../../src/xr/fiducial/rigidRegistration';

const SENSOR_POINTS: Vector3[] = [[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0], [0, 0, 1]];
type Transform = { rotation: number[]; translation: Vector3 };

function apply(transform: Transform, point: Vector3): [number, number, number] {
  const r = transform.rotation; const t = transform.translation;
  return [r[0] * point[0] + r[1] * point[1] + r[2] * point[2] + t[0], r[3] * point[0] + r[4] * point[1] + r[5] * point[2] + t[1], r[6] * point[0] + r[7] * point[1] + r[8] * point[2] + t[2]];
}

function tagsFor(transform: Transform, missing: readonly number[] = [], outlierTag?: number): FusedRegistrationTag[] {
  return SENSOR_POINTS.flatMap((sensor, tagId) => {
    if (missing.includes(tagId)) return [];
    const position = apply(transform, sensor);
    if (outlierTag === tagId) position[0] += 0.5;
    return [{ tagId, positionScan: position, spreadM: 0.001, observationCount: 5, inlierCount: 5, confirmed: true }];
  });
}

function geometry(options: { collinear?: boolean; computed?: boolean; assigned?: boolean; uncertaintyMm?: number } = {}): { pods: RegistrationPodGeometry[] } {
  const points = options.collinear ? [[1, 0, 0], [2, 0, 0], [3, 0, 0], [4, 0, 0]] : SENSOR_POINTS.slice(1);
  return { pods: points.map((p, index) => ({
    pod: index,
    computed: options.computed ?? true,
    assigned: options.assigned,
    x_mm: p[0] * 1000,
    y_mm: p[1] * 1000,
    z_mm: p[2] * 1000,
    uncertainty_mm: options.uncertaintyMm,
  })) };
}

function close(actual: number, expected: number, tolerance = 1e-9): void {
  expect(Math.abs(actual - expected)).toBeLessThanOrEqual(tolerance);
}

function firmwareUsable(matrix: readonly number[]): boolean {
  if (matrix.length !== 16 || matrix[12] !== 0 || matrix[13] !== 0 || matrix[14] !== 0 || matrix[15] !== 1) return false;
  for (let row = 0; row < 3; row += 1) for (let other = 0; other < 3; other += 1) {
    const dot = matrix[row * 4] * matrix[other * 4] + matrix[row * 4 + 1] * matrix[other * 4 + 1] + matrix[row * 4 + 2] * matrix[other * 4 + 2];
    if (Math.abs(dot - (row === other ? 1 : 0)) > 1e-3) return false;
  }
  return true;
}

describe('fiducial rigid registration', () => {
  it('decomposes full-rank and rank-deficient 3x3 matrices with svd3', () => {
    for (const matrix of [
      [3, 0, 0, 0, 2, 0, 0, 0, 1],
      [1, 2, 3, 2, 4, 6, 0, 0, 0],
    ]) {
      const { U, V, singularValues } = svd3(matrix);
      for (let row = 0; row < 3; row += 1) for (let other = 0; other < 3; other += 1) {
        const uDot = U[row * 3] * U[other * 3] + U[row * 3 + 1] * U[other * 3 + 1] + U[row * 3 + 2] * U[other * 3 + 2];
        const vDot = V[row * 3] * V[other * 3] + V[row * 3 + 1] * V[other * 3 + 1] + V[row * 3 + 2] * V[other * 3 + 2];
        close(uDot, row === other ? 1 : 0, 1e-9);
        close(vDot, row === other ? 1 : 0, 1e-9);
      }
      for (let row = 0; row < 3; row += 1) for (let column = 0; column < 3; column += 1) {
        close(matrix[row * 3 + column], [0, 1, 2].reduce((sum, k) => sum + U[row * 3 + k] * singularValues[k] * V[column * 3 + k], 0), 1e-7);
      }
    }
    expect(svd3([0, 0, 0, 0, 0, 0, 0, 0, 0]).singularValues).toEqual([0, 0, 0]);
  });

  it('recovers proper and reflected transforms from volumetric points to sub-nanometre residuals', () => {
    const angle = 37 * Math.PI / 180;
    const proper: Transform = { rotation: [Math.cos(angle), -Math.sin(angle), 0, Math.sin(angle), Math.cos(angle), 0, 0, 0, 1], translation: [0.2, -0.3, 0.1] };
    const properResult = registerSensorToScan({ tags: tagsFor(proper), geometry: geometry({ assigned: true }), geometryComputedAtMs: 1234 });
    expect(properResult.valid).toBe(true);
    expect(properResult.reason).toBeNull();
    expect(properResult.rmsResidualM).toBeLessThanOrEqual(1e-9);
    expect(properResult.mirrored).toBe(false);
    expect(properResult.tier).toBe('full');
    expect(properResult.sensorToScan).not.toBeNull();
    if (properResult.sensorToScan === null) return;
    expect(firmwareUsable(properResult.sensorToScan)).toBe(true);
    proper.rotation.forEach((value, index) => close(properResult.sensorToScan![Math.floor(index / 3) * 4 + index % 3], value));
    expect(properResult.sensorToScan[3]).toBeCloseTo(0.2, 12);
    expect(properResult.sensorToScan[7]).toBeCloseTo(-0.3, 12);
    expect(properResult.sensorToScan[11]).toBeCloseTo(0.1, 12);
    expect(properResult.sensorToScan.slice(12)).toEqual([0, 0, 0, 1]);

    const reflection: Transform = { rotation: [-1, 0, 0, 0, 1, 0, 0, 0, 1], translation: [0.1, 0.2, -0.05] };
    const reflectedResult = registerSensorToScan({ tags: tagsFor(reflection), geometry: geometry({ assigned: true }), geometryComputedAtMs: 1234 });
    expect(reflectedResult.valid).toBe(true);
    expect(reflectedResult.mirrored).toBe(true);
    expect(reflectedResult.rmsResidualM).toBeLessThanOrEqual(1e-9);
  });

  it('uses scan gravity to choose the coplanar solution that maps sensor +Z toward scan +Y', () => {
    const gravityAligned: Transform = { rotation: [1, 0, 0, 0, 0, 1, 0, -1, 0], translation: [0.1, 0.2, 0.3] };
    const result = registerSensorToScan({ tags: tagsFor(gravityAligned).filter((tag) => tag.tagId < 4), geometry: geometry({ assigned: true }), geometryComputedAtMs: 1234 });
    expect(result.valid).toBe(true);
    expect(result.rmsResidualM).toBeLessThanOrEqual(1e-9);
    expect(result.mirrored).toBe(false);
    expect(result.sensorToScan?.[6], JSON.stringify(result)).toBeCloseTo(1, 9);
  });

  it('rejects collinear layouts, bad residuals, missing MAIN, and insufficient remotes without a transform', () => {
    const identity: Transform = { rotation: [1, 0, 0, 0, 1, 0, 0, 0, 1], translation: [0, 0, 0] };
    const collinear = registerSensorToScan({ tags: tagsFor(identity), geometry: geometry({ collinear: true }), geometryComputedAtMs: 1234 });
    expect(collinear.reason).toBe('collinear');
    expect(collinear.sensorToScan).toBeNull();

    const outlier = registerSensorToScan({ tags: tagsFor(identity, [], 4), geometry: geometry(), geometryComputedAtMs: 1234 });
    expect(outlier.reason).toBe('residual_too_high');
    expect(outlier.valid).toBe(false);
    expect(outlier.rmsResidualM).toBeGreaterThan(0.05);
    expect(outlier.perNode.find((node) => node.tagId === 4)?.status).toBe('provisional');

    const noMain = registerSensorToScan({ tags: tagsFor(identity, [0]), geometry: geometry(), geometryComputedAtMs: 1234 });
    expect(noMain.reason).toBe('missing_main');
    expect(noMain.sensorToScan).toBeNull();

    const few = registerSensorToScan({ tags: tagsFor(identity, [3, 4]), geometry: geometry(), geometryComputedAtMs: 1234 });
    expect(few.reason).toBe('too_few_remotes');
    expect(few.sensorToScan).toBeNull();
    expect(few.perNode.every((node) => node.status === 'visual_only')).toBe(true);

    const noGeometry = registerSensorToScan({ tags: tagsFor(identity), geometry: geometry({ computed: false }), geometryComputedAtMs: 1234 });
    expect(noGeometry.reason).toBe('no_geometry');
    expect(noGeometry.sensorToScan).toBeNull();
  });

  it('sets minimum/full tiers and takes the greater of the uncertainty threshold and floor', () => {
    const translated: Transform = { rotation: [1, 0, 0, 0, 1, 0, 0, 0, 1], translation: [0.2, -0.3, 0.1] };
    const minimum = registerSensorToScan({ tags: tagsFor(translated, [4]), geometry: geometry({ assigned: true }), geometryComputedAtMs: 1234 });
    expect(minimum.valid).toBe(true);
    expect(minimum.tier).toBe('minimum');
    expect(minimum.perNode.every((node) => node.status === 'anchored')).toBe(true);

    const lowUncertainty = registerSensorToScan({ tags: tagsFor(translated, [4]), geometry: geometry({ assigned: true, uncertaintyMm: 10 }), geometryComputedAtMs: 1234 });
    expect(lowUncertainty.thresholdM).toBe(0.05);
    const highUncertainty = registerSensorToScan({ tags: tagsFor(translated, [4]), geometry: geometry({ assigned: true, uncertaintyMm: 100 }), geometryComputedAtMs: 1234 });
    expect(highUncertainty.thresholdM).toBeCloseTo(0.3, 12);
    expect(highUncertainty.sensorToScan?.[3]).toBeCloseTo(0.2, 12);
    expect(highUncertainty.sensorToScan?.[7]).toBeCloseTo(-0.3, 12);
    expect(highUncertainty.sensorToScan?.[11]).toBeCloseTo(0.1, 12);
  });
});

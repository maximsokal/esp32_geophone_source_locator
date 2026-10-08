import { describe, expect, it } from 'vitest';
import { registerSensorToScan, svd3, type FusedRegistrationTag, type RegistrationPodGeometry, type Vector3 } from '../../src/xr/fiducial/rigidRegistration';

const SENSOR_POINTS: Vector3[] = [[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0], [0, 0, 1]];
const NEAR_PLANAR_IDEAL_POINTS: Vector3[] = [[0, 0, 0], [1.5, 0, 0], [0, 1.5, 0], [-1.5, 0, 0], [0, -1.5, 0]];
const NEAR_PLANAR_SENSOR_POINTS: Vector3[] = [[0, 0, 0], [1.5, 0, 0.02], [0, 1.5, -0.02], [-1.5, 0, 0.02], [0, -1.5, -0.02]];
type Transform = { rotation: number[]; translation: Vector3 };

function apply(transform: Transform, point: Vector3): [number, number, number] {
  const r = transform.rotation; const t = transform.translation;
  return [r[0] * point[0] + r[1] * point[1] + r[2] * point[2] + t[0], r[3] * point[0] + r[4] * point[1] + r[5] * point[2] + t[1], r[6] * point[0] + r[7] * point[1] + r[8] * point[2] + t[2]];
}

function tagsFor(transform: Transform, missing: readonly number[] = [], outlierTag?: number, points: readonly Vector3[] = SENSOR_POINTS): FusedRegistrationTag[] {
  return points.flatMap((sensor, tagId) => {
    if (missing.includes(tagId)) return [];
    const position = apply(transform, sensor);
    if (outlierTag === tagId) position[0] += 0.5;
    return [{ tagId, positionScan: position, spreadM: 0.001, observationCount: 5, inlierCount: 5, confirmed: true }];
  });
}

function geometry(options: { collinear?: boolean; computed?: boolean; assigned?: boolean; uncertaintyMm?: number; points?: readonly Vector3[] } = {}): { pods: RegistrationPodGeometry[] } {
  const points = options.collinear ? [[1, 0, 0], [2, 0, 0], [3, 0, 0], [4, 0, 0]] : options.points ?? SENSOR_POINTS.slice(1);
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

function gravityYaw(yawDeg: number, reflected = false, translation: Vector3 = [0.2, 0.3, -0.1]): Transform {
  const angle = yawDeg * Math.PI / 180; const c = Math.cos(angle); const s = Math.sin(angle);
  return {
    rotation: reflected ? [-c, -s, 0, 0, 0, 1, s, -c, 0] : [c, -s, 0, 0, 0, 1, -s, -c, 0],
    translation,
  };
}

function rmsTransformError(matrix: readonly number[], truth: Transform, points: readonly Vector3[]): number {
  const estimate: Transform = {
    rotation: [matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6], matrix[8], matrix[9], matrix[10]],
    translation: [matrix[3], matrix[7], matrix[11]],
  };
  const squared = points.reduce((sum, point) => {
    const actual = apply(estimate, point); const expected = apply(truth, point);
    return sum + actual.reduce((pointSum, value, axis) => pointSum + (value - expected[axis]) ** 2, 0);
  }, 0);
  return Math.sqrt(squared / points.length);
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
    const proper = gravityYaw(37, false, [0.2, -0.3, 0.1]);
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

    const reflection = gravityYaw(0, true, [0.1, 0.2, -0.05]);
    const reflectedResult = registerSensorToScan({ tags: tagsFor(reflection), geometry: geometry({ assigned: true }), geometryComputedAtMs: 1234 });
    expect(reflectedResult.valid).toBe(false);
    expect(reflectedResult.reason).toBe('reflected_transform');
    expect(reflectedResult.sensorToScan).toBeNull();
  });

  it('uses gravity to select the proper near-planar fit with realistic sensor-side z noise', () => {
    const truth = gravityYaw(23);
    const result = registerSensorToScan({
      tags: tagsFor(truth, [], undefined, NEAR_PLANAR_IDEAL_POINTS),
      geometry: geometry({ assigned: true, points: NEAR_PLANAR_SENSOR_POINTS.slice(1) }),
      geometryComputedAtMs: 1234,
    });
    expect(result.valid).toBe(true);
    expect(result.mirrored).toBe(false);
    expect(result.sensorToScan).not.toBeNull();
    expect(result.sensorToScan?.[6]).toBeGreaterThan(Math.cos(30 * Math.PI / 180));
    expect(rmsTransformError(result.sensorToScan!, truth, NEAR_PLANAR_IDEAL_POINTS)).toBeLessThan(0.01);
  });

  it('never returns a mirrored near-planar fit when the sensor frame is reflected', () => {
    const truth = gravityYaw(23, true);
    const result = registerSensorToScan({
      tags: tagsFor(truth, [], undefined, NEAR_PLANAR_IDEAL_POINTS),
      geometry: geometry({ assigned: true, points: NEAR_PLANAR_SENSOR_POINTS.slice(1) }),
      geometryComputedAtMs: 1234,
    });
    expect(result.mirrored).toBe(false);
    if (result.sensorToScan !== null) {
      const matrix = result.sensorToScan;
      const determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
        matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
        matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
      expect(determinant).toBeCloseTo(1, 9);
    }
  });

  it('rejects an upright fit that would require reflecting an upside-down sensor transform', () => {
    const upsideDown: Transform = { rotation: [1, 0, 0, 0, 0, -1, 0, 1, 0], translation: [0.2, 0.3, -0.1] };
    // Exact tags from this 180-degree fit give the upside-down candidate essentially zero RMS.
    // The near-planar measurements leave an upright alternate, which gravity must prefer.
    const tags = tagsFor(upsideDown, [], undefined, NEAR_PLANAR_SENSOR_POINTS);
    const upsideDownRms = Math.sqrt(tags.reduce((sum, tag) => {
      const fitted = apply(upsideDown, NEAR_PLANAR_SENSOR_POINTS[tag.tagId]);
      return sum + tag.positionScan.reduce((pointSum, value, axis) => pointSum + (value - fitted[axis]) ** 2, 0);
    }, 0) / tags.length);
    const result = registerSensorToScan({
      tags,
      geometry: geometry({ assigned: true, points: NEAR_PLANAR_SENSOR_POINTS.slice(1) }),
      geometryComputedAtMs: 1234,
    });
    expect(upsideDownRms).toBeLessThan(1e-9);
    expect(result.valid).toBe(false);
    expect(result.reason).toBe('reflected_transform');
    expect(result.sensorToScan).toBeNull();
  });

  it('rejects a 180-degree gravity mismatch with no transform', () => {
    const upsideDown: Transform = { rotation: [1, 0, 0, 0, 0, -1, 0, 1, 0], translation: [0.2, 0.3, -0.1] };
    const points: Vector3[] = [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 2]];
    const result = registerSensorToScan({
      tags: tagsFor(upsideDown, [], undefined, points),
      geometry: geometry({ assigned: true, points: points.slice(1) }),
      geometryComputedAtMs: 1234,
    });
    expect(result.valid).toBe(false);
    expect(result.reason).toBe('gravity_inconsistent');
    expect(result.sensorToScan).toBeNull();
  });

  it('enforces the default gravity tilt boundary and accepts a wider custom limit', () => {
    const tilted = (tiltDeg: number): Transform => {
      const angle = tiltDeg * Math.PI / 180; const c = Math.cos(angle); const s = Math.sin(angle);
      return { rotation: [c, 0, -s, s, 0, c, 0, -1, 0], translation: [0.2, 0.3, -0.1] };
    };
    const register = (truth: Transform, maxTiltDeg?: number) => registerSensorToScan({
      tags: tagsFor(truth), geometry: geometry({ assigned: true }), geometryComputedAtMs: 1234, maxTiltDeg,
    });
    const accepted25 = register(tilted(25));
    expect(accepted25.valid).toBe(true);
    expect(accepted25.sensorToScan?.[6]).toBeGreaterThan(Math.cos(30 * Math.PI / 180));

    const rejected35 = register(tilted(35));
    expect(rejected35.reason).toBe('gravity_inconsistent');
    expect(rejected35.sensorToScan).toBeNull();

    const accepted35 = register(tilted(35), 40);
    expect(accepted35.valid).toBe(true);
    expect(accepted35.sensorToScan?.[6]).toBeGreaterThan(Math.cos(40 * Math.PI / 180));
  });

  it('rejects collinear layouts, bad residuals, missing MAIN, and insufficient remotes without a transform', () => {
    const gravityAligned = gravityYaw(0, false, [0, 0, 0]);
    const collinear = registerSensorToScan({ tags: tagsFor(gravityAligned), geometry: geometry({ collinear: true }), geometryComputedAtMs: 1234 });
    expect(collinear.reason).toBe('collinear');
    expect(collinear.sensorToScan).toBeNull();

    const outlier = registerSensorToScan({ tags: tagsFor(gravityAligned, [], 4), geometry: geometry(), geometryComputedAtMs: 1234 });
    expect(outlier.reason).toBe('residual_too_high');
    expect(outlier.valid).toBe(false);
    expect(outlier.rmsResidualM).toBeGreaterThan(0.05);
    expect(outlier.sensorToScan).not.toBeNull();
    expect(outlier.perNode.find((node) => node.tagId === 4)?.status).toBe('provisional');

    const noMain = registerSensorToScan({ tags: tagsFor(gravityAligned, [0]), geometry: geometry(), geometryComputedAtMs: 1234 });
    expect(noMain.reason).toBe('missing_main');
    expect(noMain.sensorToScan).toBeNull();

    const few = registerSensorToScan({ tags: tagsFor(gravityAligned, [3, 4]), geometry: geometry(), geometryComputedAtMs: 1234 });
    expect(few.reason).toBe('too_few_remotes');
    expect(few.sensorToScan).toBeNull();
    expect(few.perNode.every((node) => node.status === 'visual_only')).toBe(true);

    const noGeometry = registerSensorToScan({ tags: tagsFor(gravityAligned), geometry: geometry({ computed: false }), geometryComputedAtMs: 1234 });
    expect(noGeometry.reason).toBe('no_geometry');
    expect(noGeometry.sensorToScan).toBeNull();
  });

  it('sets minimum/full tiers and takes the greater of the uncertainty threshold and floor', () => {
    const translated = gravityYaw(0, false, [0.2, -0.3, 0.1]);
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

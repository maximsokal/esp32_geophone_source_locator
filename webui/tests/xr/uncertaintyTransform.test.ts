import { describe, expect, it } from 'vitest';

import {
  sensorSigmasToProjectHalfExtents,
  transformSensorBoxToProjectAabb,
} from '../../src/xr/uncertaintyTransform';

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

describe('sensor uncertainty transform', () => {
  it('keeps identity-registration X/Y/Z sigma extents unchanged', () => {
    expect(sensorSigmasToProjectHalfExtents([0.1, 0.2, 0.3], IDENTITY)).toEqual([0.1, 0.2, 0.3]);
  });

  it('rotates a long sensor sigma axis into the corresponding project axis', () => {
    const yaw90 = [0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    const extents = sensorSigmasToProjectHalfExtents([0.1, 1, 0.2], yaw90);
    expect(extents?.[0]).toBeCloseTo(1);
    expect(extents?.[1]).toBeCloseTo(0.1);
    expect(extents?.[2]).toBeCloseTo(0.2);
  });

  it('preserves conservative magnitudes under a reflection', () => {
    const reflection = [-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    expect(sensorSigmasToProjectHalfExtents([0.1, 0.2, 0.3], reflection)).toEqual([0.1, 0.2, 0.3]);
  });

  it('includes tilted sensor-Z uncertainty in the horizontal project extent', () => {
    const angle = Math.PI / 3;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    const localFloorRotation = [
      1, 0, 0, 0,
      0, cos, -sin, 0,
      0, sin, cos, 0,
      0, 0, 0, 1,
    ];
    // local-floor -> project uses [x, -z, y], so the sensor-Z column contributes to project Y.
    const localFloorToProject = [
      localFloorRotation[0], localFloorRotation[1], localFloorRotation[2], localFloorRotation[3],
      -localFloorRotation[8], -localFloorRotation[9], -localFloorRotation[10], -localFloorRotation[11],
      localFloorRotation[4], localFloorRotation[5], localFloorRotation[6], localFloorRotation[7],
      0, 0, 0, 1,
    ];
    const extents = sensorSigmasToProjectHalfExtents([0, 0, 2], localFloorToProject);
    expect(extents?.[1]).toBeCloseTo(Math.abs(cos * 2));
    expect(extents?.[1]).toBeGreaterThan(0);
    expect(extents?.[2]).toBeCloseTo(Math.abs(sin * 2));
  });

  it('transforms all eight credible-box corners, including registration translation', () => {
    const yawAndTranslation = [0, -1, 0, 10, 1, 0, 0, 20, 0, 0, 1, 30, 0, 0, 0, 1];
    expect(transformSensorBoxToProjectAabb([0, 0, 0], [1, 2, 3], yawAndTranslation)).toEqual({
      min: [8, 20, 30], max: [10, 21, 33],
    });
  });

  it('rejects non-finite matrices and invalid boxes', () => {
    expect(sensorSigmasToProjectHalfExtents([1, 1, 1], [...IDENTITY.slice(0, 15), Number.NaN])).toBeNull();
    expect(transformSensorBoxToProjectAabb([2, 0, 0], [1, 1, 1], IDENTITY)).toBeNull();
  });
});

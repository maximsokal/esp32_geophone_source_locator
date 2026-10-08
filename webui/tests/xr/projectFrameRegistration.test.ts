import { describe, expect, it } from 'vitest';

import type { LocalizationSessionGeometry } from '../../src/api/deviceApi';
import { toOccupancyRegistration } from '../../src/xr/fiducial/registrationArtifact';
import { encodeOccupancySolverArtifact, decodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
import { localFloorMatrixToProject } from '../../src/xr/projectFrame';
import { webXrLocalFloorToProject } from '../../src/xr/scanViewportScene';
import { buildSensorPlacementScene } from '../../src/xr/sensorPlacementScene';
import { createProjectFrameRegistration, projectRegistrationSnapshot } from './support/projectFrameRegistration';

function apply(matrix: readonly number[], point: readonly [number, number, number]): [number, number, number] {
  return [
    matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3],
    matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7],
    matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11],
  ];
}

describe('local-floor registration to project frame', () => {
  it('converts the sensor origin and yawed transforms, including Y/Z translation', () => {
    const smokingGun = [1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1, 3, 0, 0, 0, 1];
    expect(localFloorMatrixToProject(smokingGun)).toEqual([
      1, 0, 0, 0,
      0, 0, -1, -3,
      0, 1, 0, 2,
      0, 0, 0, 1,
    ]);
    expect(apply(localFloorMatrixToProject(smokingGun), [0, 0, 0])).toEqual([0, -3, 2]);

    const yaw = Math.PI / 6;
    const cosine = Math.cos(yaw);
    const sine = Math.sin(yaw);
    const localFloor = [
      cosine, -sine, 0, 1,
      0, 0, 1, 2,
      -sine, -cosine, 0, 3,
      0, 0, 0, 1,
    ];
    expect(localFloorMatrixToProject(localFloor)).toEqual([
      cosine, -sine, 0, 1,
      sine, cosine, 0, -3,
      0, 0, 1, 2,
      0, 0, 0, 1,
    ]);
    expect(() => localFloorMatrixToProject(Array(15).fill(0))).toThrow(/16/);
    expect(() => localFloorMatrixToProject([...smokingGun.slice(0, 15), Number.NaN])).toThrow(/finite/);
  });

  it('round-trips a fitted local-floor registration through the project-frame occupancy artifact', () => {
    const { result, artifact } = createProjectFrameRegistration();
    expect(result.valid).toBe(true);
    expect(result.sensorToScan).not.toBeNull();
    const registration = toOccupancyRegistration(artifact);
    expect(registration).not.toBeNull();
    if (result.sensorToScan === null || registration === null) return;

    const expectedProjectMatrix = localFloorMatrixToProject(result.sensorToScan);
    expect(registration.frame).toBe('project_z_up');
    expectedProjectMatrix.forEach((value, index) => {
      expect(Math.abs(registration.sensor_to_scan[index] - value)).toBeLessThanOrEqual(1e-12);
    });

    const bytes = encodeOccupancySolverArtifact(projectRegistrationSnapshot(), {
      frameId: 'scan-crc32:deadbeef',
      createdAtMs: 1700000000000,
      registration,
    });
    const decoded = decodeOccupancySolverArtifact(bytes);
    expect(decoded.header.schema).toBe(2);
    expect(decoded.header.registration?.frame).toBe('project_z_up');
    expect(decoded.header.registration?.sensor_to_scan).toEqual(expectedProjectMatrix);

    for (const point of [[1, 2, 3], [-0.5, 0.25, 2], [0, 0, 0]] as const) {
      const localFloorPoint = apply(result.sensorToScan, point);
      const expected = webXrLocalFloorToProject(localFloorPoint[0], localFloorPoint[1], localFloorPoint[2]);
      const actual = apply(decoded.header.registration!.sensor_to_scan, point);
      actual.forEach((coordinate, axis) => {
        expect(Math.abs(coordinate - expected[axis])).toBeLessThanOrEqual(1e-9);
      });
    }

    const sensorPoint: [number, number, number] = [1, 2, 3];
    const geometry: LocalizationSessionGeometry = {
      available: true,
      pods: [{
        pod: 0,
        node_id: 1,
        computed: true,
        assigned: true,
        leveled: true,
        position_source: 'channel_sounding',
        x_mm: sensorPoint[0] * 1000,
        y_mm: sensorPoint[1] * 1000,
        z_mm: sensorPoint[2] * 1000,
        uncertainty_mm: 40,
        residual_mm: 0,
      }],
    };
    const marker = buildSensorPlacementScene(geometry, artifact).markers.find((item) => item.node === 'G1');
    const occupancyPoint = apply(decoded.header.registration!.sensor_to_scan, sensorPoint);
    expect(marker?.positionScanM).not.toBeNull();
    marker!.positionScanM!.forEach((coordinate, axis) => {
      expect(Math.abs(coordinate - occupancyPoint[axis])).toBeLessThanOrEqual(1e-9);
    });
  });
});

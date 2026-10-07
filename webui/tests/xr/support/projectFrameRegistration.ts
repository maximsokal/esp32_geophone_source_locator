import { defaultRegistrationTagSizes, toOccupancyRegistration } from '../../../src/xr/fiducial/registrationArtifact';
import { registrationArtifactFromResult } from '../../../src/xr/fiducial/tagModeState';
import { fuseTagObservations } from '../../../src/xr/fiducial/tagObservationFusion';
import { registerSensorToScan, type RegistrationPodGeometry, type Vector3 } from '../../../src/xr/fiducial/rigidRegistration';

const SENSOR_POINTS: Vector3[] = [
  [0, 0, 0],
  [1, 0, 0],
  [0, 1, 0],
  [1, 1, 0],
  [0, 0, 1],
];

export function createProjectFrameRegistration() {
  const yaw = Math.PI / 2;
  const cosine = Math.cos(yaw);
  const sine = Math.sin(yaw);
  const rotation = [cosine, -sine, 0, 0, 0, 1, -sine, -cosine, 0];
  const translation: Vector3 = [1, 2, 3];
  const observations = SENSOR_POINTS.flatMap((point, tagId) => {
    const positionScan: [number, number, number] = [
      rotation[0] * point[0] + rotation[1] * point[1] + rotation[2] * point[2] + translation[0],
      rotation[3] * point[0] + rotation[4] * point[1] + rotation[5] * point[2] + translation[1],
      rotation[6] * point[0] + rotation[7] * point[1] + rotation[8] * point[2] + translation[2],
    ];
    return Array.from({ length: 5 }, (_, view) => ({
      tagId,
      positionScan,
      cameraPosition: [view * 0.3, 0, 0] as const,
      cameraForward: [0, 0, 1] as const,
    }));
  });
  const fusion = fuseTagObservations(observations);
  const geometry = {
    pods: SENSOR_POINTS.slice(1).map((point, index): RegistrationPodGeometry => ({
      pod: index,
      computed: true,
      assigned: true,
      x_mm: point[0] * 1000,
      y_mm: point[1] * 1000,
      z_mm: point[2] * 1000,
    })),
  };
  const result = registerSensorToScan({
    tags: Object.values(fusion.byTagId).filter((tag) => tag !== undefined),
    geometry,
    geometryComputedAtMs: 123456,
  });
  if (!result.valid || result.sensorToScan === null) throw new Error('Test registration fixture did not produce a valid transform.');
  const artifact = registrationArtifactFromResult(result, fusion, defaultRegistrationTagSizes(), 123456, 1700000000000);
  const occupancyRegistration = toOccupancyRegistration(artifact);
  if (occupancyRegistration === null) throw new Error('Test registration fixture did not adapt to occupancy.');
  return { result, artifact, occupancyRegistration };
}

export function projectRegistrationSnapshot() {
  return {
    freeAirPositions: new Float32Array([0.99, -2.97, 2.07]),
    occupiedSurfacePositions: new Float32Array([0.99, 2.07, 2.97]),
    occupancyEnvelopeBounds: { min: [0, -4, 1] as const, max: [2, 3, 4] as const },
    occupancySolverCandidateBelowSurfaceMeters: 1,
    occupancyCellsSaturated: false,
    occupancyFineVoxelSizeMeters: 0.18,
    occupancyVoxelSizeMeters: 0.18,
  };
}

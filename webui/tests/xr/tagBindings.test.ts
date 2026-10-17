import { describe, expect, it } from 'vitest';
import { fuseTagObservations } from '../../src/xr/fiducial/tagObservationFusion';
import { registerSensorToScan, type FusedRegistrationTag, type RegistrationPodGeometry, type Vector3 } from '../../src/xr/fiducial/rigidRegistration';
import { defaultRegistrationTagSizes } from '../../src/xr/fiducial/registrationArtifact';
import { registrationArtifactFromResult, tagChipStatuses } from '../../src/xr/fiducial/tagModeState';
import { TAG_BINDINGS, TAG_ID_TO_NODE, NODE_TO_TAG_ID, tagSizeMmForId, type TagBinding } from '../../src/xr/fiducial/tagBindings.gen';

const swappedBindings: readonly TagBinding[] = TAG_BINDINGS.map((binding) => ({
  ...binding,
  node: binding.tag_id === 0 ? 'G1' : binding.tag_id === 1 ? 'MAIN' : binding.node,
}));

const sensorPositions: readonly Vector3[] = [[1, 0, 0], [0, 1, 0], [1, 1, 0], [0, 0, 1]];

function scanPosition(sensor: Vector3): Vector3 {
  return [sensor[0] + 0.2, sensor[2] + 0.3, -sensor[1] - 0.1];
}

describe('generated AprilTag bindings', () => {
  it('exports both directions and per-id physical sizes for the default sheet', () => {
    expect(TAG_ID_TO_NODE).toEqual({ 0: 'MAIN', 1: 'G1', 2: 'G2', 3: 'G3', 4: 'G4' });
    expect(NODE_TO_TAG_ID).toEqual({ MAIN: 0, G1: 1, G2: 2, G3: 3, G4: 4 });
    expect(TAG_BINDINGS.every((binding) => binding.size_mm === 32)).toBe(true);
    expect(tagSizeMmForId(3)).toBe(32);
    expect(defaultRegistrationTagSizes()).toEqual({ '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 });
    expect(defaultRegistrationTagSizes(0.04, 0.03)).toEqual({ '0': 0.04, '1': 0.03, '2': 0.03, '3': 0.03, '4': 0.03 });
  });

  it('uses a swapped binding for chip labels and rigid registration node assignments', () => {
    const fusion = fuseTagObservations([0, 1, 2, 3, 4].map((tagId) => ({ tagId, positionScan: [tagId, 0, 0] as const })));
    expect(tagChipStatuses(fusion, 'en', swappedBindings).map(({ tagId, node }) => [tagId, node])).toEqual([
      [0, 'G1'], [1, 'MAIN'], [2, 'G2'], [3, 'G3'], [4, 'G4'],
    ]);

    const pods: RegistrationPodGeometry[] = sensorPositions.map((position, pod) => ({
      pod, computed: true, assigned: true,
      x_mm: position[0] * 1000, y_mm: position[1] * 1000, z_mm: position[2] * 1000,
    }));
    const tags: FusedRegistrationTag[] = swappedBindings.map((binding) => {
      const sensor = binding.node === 'MAIN' ? [0, 0, 0] as const : sensorPositions[Number(binding.node.slice(1)) - 1];
      return { tagId: binding.tag_id, positionScan: scanPosition(sensor), spreadM: 0, observationCount: 5, inlierCount: 5, confirmed: true };
    });
    const result = registerSensorToScan({ tags, geometry: { pods }, geometryComputedAtMs: 1234, bindings: swappedBindings });
    expect(result.valid).toBe(true);
    expect(result.perNode.map(({ tagId, node, status }) => [tagId, node, status])).toEqual([
      [0, 'G1', 'anchored'], [1, 'MAIN', 'anchored'], [2, 'G2', 'anchored'], [3, 'G3', 'anchored'], [4, 'G4', 'anchored'],
    ]);

    const artifact = registrationArtifactFromResult(result, fusion, defaultRegistrationTagSizes(), 1234, 5678, null, swappedBindings);
    expect(artifact.bindings.map(({ tag_id, node }) => [tag_id, node])).toEqual([
      [0, 'G1'], [1, 'MAIN'], [2, 'G2'], [3, 'G3'], [4, 'G4'],
    ]);
  });
});

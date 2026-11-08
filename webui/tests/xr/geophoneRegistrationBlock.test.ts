import { describe, expect, it } from 'vitest';
import {
  bleVsTagResiduals,
  geophonePositionsFromTags,
  geophonePositionsRegistrationBlock,
} from '../../src/xr/fiducial/geophonePositions';
import { decodeRegistrationArtifact, defaultRegistrationTagSizes, encodeRegistrationArtifact, type RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import type { FusedRegistrationTag } from '../../src/xr/fiducial/rigidRegistration';
import { registrationArtifactFromResult } from '../../src/xr/fiducial/tagModeState';
import { registerSensorToScan } from '../../src/xr/fiducial/rigidRegistration';

// WebXR local-floor, +Y up, metres. G1..G4 on a 1 m square.
const CENTRES: Record<number, [number, number, number]> = {
  1: [0.1, 0.02, 0.2],
  2: [1.1, 0.02, 0.2],
  3: [1.1, 0.02, 1.2],
  4: [0.1, 0.02, 1.2],
};

function tag(tagId: number): FusedRegistrationTag {
  return { tagId, positionScan: [...CENTRES[tagId]], spreadM: 0.002, observationCount: 12, inlierCount: 11, confirmed: true };
}

const TAGS = [1, 2, 3, 4].map(tag);

function baseArtifact(): RegistrationArtifact {
  const fusion = { byTagId: Object.fromEntries(TAGS.map((t) => [t.tagId, { ...t, independentViewCount: 4 }])), rejectedCount: 0 };
  const result = registerSensorToScan({ tags: TAGS, geometry: { pods: [], available: false }, geometryComputedAtMs: 4000 });
  return registrationArtifactFromResult(result, fusion, defaultRegistrationTagSizes(), 4000, 5000);
}

function withBlock(): RegistrationArtifact {
  const positions = geophonePositionsFromTags({ tags: TAGS });
  const block = geophonePositionsRegistrationBlock(positions);
  if (block === null) throw new Error('block expected');
  return { ...baseArtifact(), geophone_positions: block };
}

describe('REMOTE_ONLY registration block with the geophone positions', () => {
  it('converts the tag centres to the project frame in millimetres without an offset', () => {
    const block = geophonePositionsRegistrationBlock(geophonePositionsFromTags({ tags: TAGS }));
    expect(block?.source).toBe('APRILTAG_CENTER_APPROX');
    expect(block?.frame).toBe('project_z_up_mm');
    // [x, -z, y] * 1000: local-floor +Y (up) becomes project +Z, local-floor Z becomes project -Y.
    expect(block?.positions).toEqual([
      { node_id: 1, tag_id: 1, x_mm: 100, y_mm: -200, z_mm: 20 },
      { node_id: 2, tag_id: 2, x_mm: 1100, y_mm: -200, z_mm: 20 },
      { node_id: 3, tag_id: 3, x_mm: 1100, y_mm: -1200, z_mm: 20 },
      { node_id: 4, tag_id: 4, x_mm: 100, y_mm: -1200, z_mm: 20 },
    ]);
  });

  it('never writes a negative zero and never an empty block', () => {
    const flat = geophonePositionsFromTags({ tags: [1, 2].map((id) => ({ ...tag(id), positionScan: [0, 0, 0.0004] as [number, number, number] })) });
    const block = geophonePositionsRegistrationBlock(flat);
    expect(block?.positions.every((p) => [p.x_mm, p.y_mm, p.z_mm].every((v) => !Object.is(v, -0)))).toBe(true);
    expect(geophonePositionsRegistrationBlock(geophonePositionsFromTags({ tags: [] }))).toBeNull();
  });

  it('carries only the accepted geophones and the BLE residual report', () => {
    const positions = geophonePositionsFromTags({ tags: [1, 2, 4].map(tag) }); // no tag 3
    const residuals = bleVsTagResiduals(positions, [{ nodeA: 1, nodeB: 2, distanceMm: 900, valid: true }]);
    const block = geophonePositionsRegistrationBlock(positions, residuals);
    expect(block?.positions.map((p) => p.node_id)).toEqual([1, 2, 4]);
    const pair12 = block?.ble_residuals?.find((r) => r.node_a === 1 && r.node_b === 2);
    expect(pair12).toEqual({ node_a: 1, node_b: 2, tag_distance_mm: 1000, ble_distance_mm: 900, residual_mm: 100 });
    expect(block?.ble_residuals?.find((r) => r.node_a === 1 && r.node_b === 4)?.residual_mm).toBeNull(); // no BLE distance: null, not 0
  });

  it('is attached to the artifact only when given, so the MAIN-based artifact is unchanged', () => {
    const legacy = baseArtifact();
    expect('geophone_positions' in legacy).toBe(false);
    expect(Object.keys(JSON.parse(encodeRegistrationArtifact(legacy)) as object)).not.toContain('geophone_positions');
    const positions = geophonePositionsFromTags({ tags: TAGS });
    const fusion = { byTagId: Object.fromEntries(TAGS.map((t) => [t.tagId, { ...t, independentViewCount: 4 }])), rejectedCount: 0 };
    const result = registerSensorToScan({ tags: TAGS, geometry: { pods: [], available: false }, geometryComputedAtMs: 4000 });
    const artifact = registrationArtifactFromResult(result, fusion, defaultRegistrationTagSizes(), 4000, 5000, null, undefined,
      geophonePositionsRegistrationBlock(positions));
    expect(artifact.geophone_positions?.positions).toHaveLength(4);
  });

  it('round-trips through encode and decode', () => {
    const artifact = withBlock();
    const decoded = decodeRegistrationArtifact(encodeRegistrationArtifact(artifact));
    expect(decoded.geophone_positions).toEqual(artifact.geophone_positions);
    // The legacy artifact still decodes, with no block invented.
    expect(decodeRegistrationArtifact(encodeRegistrationArtifact(baseArtifact())).geophone_positions).toBeUndefined();
  });

  it.each([
    ['unknown source', (b: Record<string, unknown>) => { b.source = 'GPS'; }],
    ['unknown frame', (b: Record<string, unknown>) => { b.frame = 'webxr_local_floor'; }],
    ['tag of another geophone', (b: Record<string, unknown>) => { (b.positions as Record<string, unknown>[])[1].tag_id = 1; }],
    ['node 5', (b: Record<string, unknown>) => { (b.positions as Record<string, unknown>[])[0].node_id = 5; (b.positions as Record<string, unknown>[])[0].tag_id = 5; }],
    ['repeated node', (b: Record<string, unknown>) => { (b.positions as Record<string, unknown>[])[1].node_id = 1; (b.positions as Record<string, unknown>[])[1].tag_id = 1; }],
    ['fraction of a millimetre', (b: Record<string, unknown>) => { (b.positions as Record<string, unknown>[])[0].x_mm = 0.5; }],
    ['out of range', (b: Record<string, unknown>) => { (b.positions as Record<string, unknown>[])[0].z_mm = 2_000_000; }],
    ['no positions', (b: Record<string, unknown>) => { b.positions = []; }],
    ['extra field', (b: Record<string, unknown>) => { b.offset_mm = 5; }],
    ['residual without a BLE distance', (b: Record<string, unknown>) => { b.ble_residuals = [{ node_a: 1, node_b: 2, tag_distance_mm: 1000, ble_distance_mm: null, residual_mm: 5 }]; }],
  ])('refuses a block with %s', (_name, damage) => {
    const json = JSON.parse(encodeRegistrationArtifact(withBlock())) as Record<string, unknown>;
    damage(json.geophone_positions as Record<string, unknown>);
    expect(() => decodeRegistrationArtifact(JSON.stringify(json))).toThrow();
  });
});

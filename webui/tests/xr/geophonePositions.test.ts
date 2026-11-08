import { describe, expect, it } from 'vitest';
import {
  GEOPHONE_POSITION_SOURCE,
  bleVsTagResiduals,
  geophonePositionsFromTags,
  type BleDistanceInput,
} from '../../src/xr/fiducial/geophonePositions';
import { registerSensorToScan, type FusedRegistrationTag } from '../../src/xr/fiducial/rigidRegistration';
import { TAG_BINDINGS, tagIdForNode, tagSizeMmForId, type TagBinding } from '../../src/xr/fiducial/tagBindings.gen';

// A 1 m square of tag centres in WebXR local-floor (+Y up): G1..G4 on the floor plane, tag 0 on the housing.
const CENTRES: Record<number, [number, number, number]> = {
  0: [2.0, 0.3, 0.5],
  1: [0.1, 0.02, 0.2],
  2: [1.1, 0.02, 0.2],
  3: [1.1, 0.02, 1.2],
  4: [0.1, 0.02, 1.2],
};

function tag(tagId: number, overrides: Partial<FusedRegistrationTag> = {}): FusedRegistrationTag {
  return { tagId, positionScan: [...CENTRES[tagId]], spreadM: 0.002, observationCount: 12, inlierCount: 11, confirmed: true, ...overrides };
}

const REMOTE_TAGS = [1, 2, 3, 4].map((id) => tag(id));

describe('AprilTag centres as approximate geophone positions (REMOTE_ONLY)', () => {
  it('remote_only_tag_bindings_1_to_4', () => {
    // The existing contract, read from the generated bindings: G1..G4 are tags 1..4, 32 mm, no second set of tags.
    expect((['G1', 'G2', 'G3', 'G4'] as const).map((node) => tagIdForNode(node))).toEqual([1, 2, 3, 4]);
    expect([1, 2, 3, 4].map((id) => tagSizeMmForId(id))).toEqual([32, 32, 32, 32]);
    expect(TAG_BINDINGS).toHaveLength(5);
    const result = geophonePositionsFromTags({ tags: REMOTE_TAGS });
    expect(result.positions.map((p) => [p.node, p.tagId])).toEqual([['G1', 1], ['G2', 2], ['G3', 3], ['G4', 4]]);
  });

  it('tag_0_not_required_for_geophones', () => {
    const result = geophonePositionsFromTags({ tags: REMOTE_TAGS }); // no tag 0 at all
    expect(result.complete).toBe(true);
    expect(result.rejected).toEqual([]);
    expect(result.housingLandmark).toBeNull();
    // A bad tag 0 does not touch the geophones either.
    const withBadHousing = geophonePositionsFromTags({ tags: [...REMOTE_TAGS, tag(0, { confirmed: false, spreadM: 1 })] });
    expect(withBadHousing.complete).toBe(true);
    expect(withBadHousing.positions).toHaveLength(4);
  });

  it('tag_pose_center_is_accepted_without_offset', () => {
    const result = geophonePositionsFromTags({ tags: REMOTE_TAGS });
    for (const id of [1, 2, 3, 4]) {
      const position = result.positions.find((p) => p.tagId === id);
      expect(position?.positionScan).toEqual(CENTRES[id]); // exactly the tag centre: offset 0 by design
      expect(position?.source).toBe(GEOPHONE_POSITION_SOURCE);
      expect(position?.source).toBe('APRILTAG_CENTER_APPROX');
    }
  });

  it('four_remote_tags_produce_four_scan_positions', () => {
    const result = geophonePositionsFromTags({ tags: REMOTE_TAGS });
    expect(result.complete).toBe(true);
    expect(Object.keys(result.byNode).sort()).toEqual(['G1', 'G2', 'G3', 'G4']);
    // The tag size comes from the binding, not from a number written here.
    const custom: TagBinding[] = TAG_BINDINGS.map((b) => ({ ...b, size_mm: b.tag_id === 3 ? 40 : b.size_mm }));
    const sized = geophonePositionsFromTags({ tags: REMOTE_TAGS, bindings: custom });
    expect(sized.byNode.G3?.tagSizeMm).toBe(40);
    expect(sized.byNode.G1?.tagSizeMm).toBe(32);
  });

  it('missing_tag_does_not_create_zero_position', () => {
    const result = geophonePositionsFromTags({ tags: [1, 2, 4].map((id) => tag(id)) });
    expect(result.complete).toBe(false);
    expect(result.byNode.G3).toBeUndefined();
    expect(result.positions).toHaveLength(3);
    expect(result.positions.some((p) => p.positionScan.every((value) => value === 0))).toBe(false);
    expect(result.rejected).toEqual([{ node: 'G3', tagId: 3, reason: 'missing' }]);
  });

  it('bad_pose_or_old_scan_rejected', () => {
    const unconfirmed = geophonePositionsFromTags({ tags: [tag(1), tag(2, { confirmed: false }), tag(3), tag(4)] });
    expect(unconfirmed.byNode.G2).toBeUndefined();
    expect(unconfirmed.rejected).toContainEqual({ node: 'G2', tagId: 2, reason: 'unconfirmed' });

    const wobbly = geophonePositionsFromTags({ tags: [tag(1), tag(2), tag(3, { spreadM: 0.2 }), tag(4)] });
    expect(wobbly.byNode.G3).toBeUndefined();
    expect(wobbly.rejected).toContainEqual({ node: 'G3', tagId: 3, reason: 'spread_too_high' });

    const broken = geophonePositionsFromTags({ tags: [tag(1), tag(2), tag(3), tag(4, { positionScan: [Number.NaN, 0, 0] })] });
    expect(broken.byNode.G4).toBeUndefined();
    expect(broken.rejected).toContainEqual({ node: 'G4', tagId: 4, reason: 'invalid_position' });

    // Old: tag 1 was last seen 20 s before "now"; the others a second ago. The old one is not reused silently.
    const stale = geophonePositionsFromTags({
      tags: REMOTE_TAGS, nowMs: 100_000, maxAgeMs: 10_000, lastSeenAtMs: { 1: 80_000, 2: 99_000, 3: 99_500, 4: 99_900 },
    });
    expect(stale.byNode.G1).toBeUndefined();
    expect(stale.rejected).toContainEqual({ node: 'G1', tagId: 1, reason: 'stale' });
    expect(stale.positions).toHaveLength(3);

    // A pose from another scan is not a position in this one.
    const otherScan = geophonePositionsFromTags({ tags: REMOTE_TAGS, scanFrameId: 'scan-crc32:aaaa0001', expectedScanFrameId: 'scan-crc32:bbbb0002' });
    expect(otherScan.positions).toHaveLength(0);
    expect(otherScan.rejected.map((r) => r.reason)).toEqual(['scan_mismatch', 'scan_mismatch', 'scan_mismatch', 'scan_mismatch']);
    const sameScan = geophonePositionsFromTags({ tags: REMOTE_TAGS, scanFrameId: 'scan-crc32:aaaa0001', expectedScanFrameId: 'scan-crc32:aaaa0001' });
    expect(sameScan.complete).toBe(true);
    expect(sameScan.scanFrameId).toBe('scan-crc32:aaaa0001');
  });

  it('ble_vs_tag_distances_report_residual', () => {
    const result = geophonePositionsFromTags({ tags: REMOTE_TAGS });
    const ble: BleDistanceInput[] = [
      { nodeA: 1, nodeB: 2, distanceMm: 1000, valid: true },   // tags are exactly 1000 mm apart: the residual is 0 because they agree
      { nodeA: 2, nodeB: 3, distanceMm: 1300, valid: true },   // tags 1000 mm apart: -300
      { nodeA: 1, nodeB: 3, distanceMm: 0, valid: false },     // not measured: no residual, not "0 minus 1414"
      { nodeA: 4, nodeB: 3, distanceMm: 700, valid: true },    // reversed order is the same pair; tags 1000 apart: +300
    ];
    const residuals = bleVsTagResiduals(result, ble);
    const byPair = new Map(residuals.map((r) => [`${r.nodeA}-${r.nodeB}`, r]));
    expect(residuals).toHaveLength(6); // all six pairs of four geophones are reported
    expect(byPair.get('G1-G2')?.residualMm).toBeCloseTo(0, 3);
    expect(byPair.get('G2-G3')?.residualMm).toBeCloseTo(-300, 3);
    expect(byPair.get('G3-G4')?.residualMm).toBeCloseTo(300, 3);
    expect(byPair.get('G1-G3')?.bleDistanceMm).toBeNull();
    expect(byPair.get('G1-G3')?.residualMm).toBeNull();
    expect(byPair.get('G1-G3')?.tagDistanceMm).toBeGreaterThan(1400); // the diagonal of the square, 1.414 m
    expect(byPair.get('G1-G4')?.residualMm).toBeNull(); // no BLE entry at all
    // The residuals are reported, never used to move a tag centre.
    expect(result.byNode.G2?.positionScan).toEqual(CENTRES[2]);
  });

  it('legacy_tag0_still_supported', () => {
    const housing = geophonePositionsFromTags({ tags: [tag(0), ...REMOTE_TAGS] });
    expect(housing.housingLandmark).toEqual({ tagId: 0, positionScan: CENTRES[0] });
    expect(housing.positions.map((p) => p.tagId)).toEqual([1, 2, 3, 4]); // tag 0 is never a geophone
    // The MAIN-based registration is untouched: with tag 0 and the sensor geometry it still fits a transform.
    const registration = registerSensorToScan({
      tags: [tag(0, { positionScan: [0, 0, 0] }), tag(1, { positionScan: [1, 0, 0] }), tag(2, { positionScan: [0, 0, 1] }), tag(3, { positionScan: [1, 0, 1] })],
      geometry: { pods: [
        { pod: 0, computed: true, x_mm: 1000, y_mm: 0, z_mm: 0, uncertainty_mm: 100 },
        { pod: 1, computed: true, x_mm: 0, y_mm: 1000, z_mm: 0, uncertainty_mm: 100 },
        { pod: 2, computed: true, x_mm: 1000, y_mm: 1000, z_mm: 0, uncertainty_mm: 100 },
      ] },
      geometryComputedAtMs: 1,
    });
    expect(registration.reason).not.toBe('missing_main');
  });
});

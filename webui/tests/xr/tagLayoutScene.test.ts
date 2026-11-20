import { describe, expect, it } from 'vitest';
import type { TagSensorAnchor } from '../../src/xr/fiducial/liveTagTracker';
import { buildTagLayout } from '../../src/xr/fiducial/tagLayout';
import { tagLayoutMarkers, tagLayoutRegistration, tagLayoutSensorToLocalFloor, tagNodeName } from '../../src/xr/fiducial/tagLayoutScene';
import { tagPositionsFromLayout } from '../../src/xr/fiducial/tagPositionsArtifact';
import { registrationApplicability, registrationTransformIssue } from '../../src/xr/registrationApplicability';
import { sourcePositionToProject } from '../../src/xr/sourceHighlight';

const anchor = (tagId: number, node: string, xr: [number, number, number], confirmed = true): TagSensorAnchor =>
  ({ tagId, node, positionScan: xr, spreadM: 0.003, observationCount: 8, independentViewCount: 4, confirmed });
const layout = buildTagLayout([anchor(0, 'MAIN', [1, 1, -1]), anchor(1, 'G1', [1.5, 1, -1]), anchor(2, 'G2', [1, 1, -1.8], false), anchor(3, 'G3', [0.9, 1.2, -1.5])]).layout!;
const artifact = (scanId: string | null = 'scan-crc32:732740d9') => tagPositionsFromLayout(layout, scanId, 1_700_000_000_000);

describe('tag layout in the 3D scan view', () => {
  it('puts every node at layout + MAIN in the scan, with its tag, spread and a provisional flag for the unconfirmed ones', () => {
    const markers = tagLayoutMarkers(artifact());
    const byNode = new Map(markers.map((marker) => [marker.node, marker]));
    expect(markers.map((marker) => marker.node)).toEqual(['MAIN', 'G1', 'G2', 'G3']);
    expect(byNode.get('MAIN')?.positionScanM).toEqual([1, 1, 1]);
    expect(byNode.get('G1')?.positionScanM).toEqual([1.5, 1, 1]);
    expect(byNode.get('G2')?.positionScanM).toEqual([1, 1.8, 1]);
    expect(byNode.get('G3')?.positionScanM).toEqual([0.9, 1.5, 1.2]);
    expect(byNode.get('G1')).toMatchObject({ source: 'apriltag', status: 'visual_only', tagId: 1, uncertaintyM: 0.004 });
    expect(byNode.get('G1')?.provisional).toBeUndefined();
    expect(byNode.get('G2')?.provisional).toBe(true);
  });

  it('maps node ids to names and refuses ids outside the five devices', () => {
    expect([0, 1, 4].map(tagNodeName)).toEqual(['MAIN', 'G1', 'G4']);
    expect([5, -1, 1.5].map(tagNodeName)).toEqual([null, null, null]);
  });

  it('carries a sensor-frame result into the scan by translation to MAIN only, through the shared source transform', () => {
    const matrix = tagLayoutSensorToLocalFloor(artifact());
    expect(registrationTransformIssue(matrix)).toBeNull();           // a proper rotation, not a reflection
    const registration = tagLayoutRegistration(artifact(), 4242)!;
    expect(registration.valid).toBe(true);
    const to = (x: number, y: number, z: number) => sourcePositionToProject({ x_m: x, y_m: y, z_m: z }, registration);
    expect(to(0, 0, 0)).toEqual([1, 1, 1]);                          // MAIN's own position
    const moved = to(0.5, 0.25, -0.1)!;
    expect(moved[0]).toBeCloseTo(1.5, 9);                            // x stays x
    expect(moved[1]).toBeCloseTo(1.25, 9);                           // y stays y: not swapped with z, not mirrored
    expect(moved[2]).toBeCloseTo(0.9, 9);                            // z stays up
  });

  it('is applicable to the scan it names and to the geometry generation of the run, and to nothing else', () => {
    const registration = tagLayoutRegistration(artifact(), 4242);
    expect(registrationApplicability(registration, { geometryComputedAtMs: 4242, scanFrameId: 'scan-crc32:732740d9' })).toEqual({ ok: true });
    expect(registrationApplicability(registration, { geometryComputedAtMs: 4243, scanFrameId: 'scan-crc32:732740d9' })).toEqual({ ok: false, reason: 'geometry_mismatch' });
    expect(registrationApplicability(registration, { geometryComputedAtMs: 4242, scanFrameId: 'scan-crc32:00000000' })).toEqual({ ok: false, reason: 'scan_mismatch' });
  });

  it('gives no registration for a layout that does not name its scan or a run without a geometry generation', () => {
    expect(tagLayoutRegistration(artifact(null), 4242)).toBeNull();
    expect(tagLayoutRegistration(artifact(), Number.NaN)).toBeNull();
  });
});

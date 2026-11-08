import { describe, expect, it } from 'vitest';

import { defaultRegistrationTagSizes, type RegistrationArtifact, type RegistrationGeophonePositions } from '../../src/xr/fiducial/registrationArtifact';
import { bindRegistrationToScan, decideContinuationRegistration } from '../../src/xr/registrationCompose';

/*
 * A scan is kept with its recording. When the operator comes back and extends it (a Continue scan in a new WebXR session),
 * the geophone tag centres seen in the extension join the ones the base scan already had, moved into the base frame.
 * Nothing about a MAIN-based registration changes.
 */

const MERGED = 'scan-crc32:00c0ffee';
const T_BA = { yaw: Math.PI / 2, tx: 1, ty: 2, tz: 0.5 }; // B -> A in the project frame, metres

function block(entries: Array<[number, number, number, number]>): RegistrationGeophonePositions {
  return {
    source: 'APRILTAG_CENTER_APPROX',
    frame: 'project_z_up_mm',
    positions: entries.map(([id, x, y, z]) => ({ node_id: id as 1 | 2 | 3 | 4, tag_id: id as 1 | 2 | 3 | 4, x_mm: x, y_mm: y, z_mm: z })),
  };
}

/** The registration the tag panel makes for a REMOTE_ONLY session: no MAIN fit (invalid), but the tag centres. */
function remoteOnlyArtifact(positions: RegistrationGeophonePositions | undefined, scanFrameId: string | null = null): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: scanFrameId, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: 4000, created_at_ms: 5000, tag_size_m: defaultRegistrationTagSizes(),
    bindings: [{ tag_id: 2, node: 'G2', observation_count: 8, inlier_count: 8, position_scan_m: [1, 0.02, -2], spread_m: 0.002 }],
    sensor_to_scan: null, mirrored: false, residual_rms_m: 0, threshold_m: 0.05,
    per_node: [{ node: 'G2', tagId: 2, residualM: null, status: 'visual_only' }], tier: null, valid: false, reason: 'missing_main',
    ...(positions === undefined ? {} : { geophone_positions: positions }),
  };
}

function toBaseMm(x: number, y: number, z: number): [number, number, number] {
  const c = Math.cos(T_BA.yaw), s = Math.sin(T_BA.yaw);
  return [Math.round(c * x - s * y + T_BA.tx * 1000) + 0, Math.round(s * x + c * y + T_BA.ty * 1000) + 0, Math.round(z + T_BA.tz * 1000) + 0];
}

describe('REMOTE_ONLY continuation scan', () => {
  it('moves the tag centres of the extension into the base frame when there is no base registration', () => {
    const extension = remoteOnlyArtifact(block([[2, 1000, 0, 20], [3, 1000, -1000, 20]]));
    const decision = decideContinuationRegistration({ base: null, artifactB: extension, extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(decision.kind).toBe('composed');
    if (decision.kind !== 'composed') return;
    expect(decision.artifact.scan_frame_id).toBe(MERGED);
    expect(decision.artifact.valid).toBe(false); // still no MAIN fit
    const [g2, g3] = decision.artifact.geophone_positions?.positions ?? [];
    expect([g2.x_mm, g2.y_mm, g2.z_mm]).toEqual(toBaseMm(1000, 0, 20));
    expect([g3.x_mm, g3.y_mm, g3.z_mm]).toEqual(toBaseMm(1000, -1000, 20));
    for (const p of decision.artifact.geophone_positions?.positions ?? []) {
      expect([p.x_mm, p.y_mm, p.z_mm].every((v) => Number.isInteger(v) && !Object.is(v, -0))).toBe(true);
    }
  });

  it('keeps the base positions and adds the geophones only the extension saw', () => {
    const base = remoteOnlyArtifact(block([[1, 100, 200, 20], [2, 1100, 200, 20]]), 'scan-crc32:aaaa0001');
    const extension = remoteOnlyArtifact(block([[2, 5000, 5000, 5000], [3, 1000, -1000, 20], [4, 0, -1000, 20]]));
    const decision = decideContinuationRegistration({ base, artifactB: extension, extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(decision.kind).toBe('kept_base');
    const positions = decision.kind === 'none' ? [] : decision.artifact.geophone_positions?.positions ?? [];
    expect(positions.map((p) => p.node_id)).toEqual([1, 2, 3, 4]);
    expect([positions[0].x_mm, positions[0].y_mm]).toEqual([100, 200]);
    expect([positions[1].x_mm, positions[1].y_mm, positions[1].z_mm]).toEqual([1100, 200, 20]); // the base wins for G2
    expect([positions[2].x_mm, positions[2].y_mm, positions[2].z_mm]).toEqual(toBaseMm(1000, -1000, 20));
    expect(decision.kind === 'none' ? null : decision.artifact.scan_frame_id).toBe(MERGED);
  });

  it('keeps the base positions when the extension saw no tags', () => {
    const base = remoteOnlyArtifact(block([[1, 100, 200, 20], [2, 1100, 200, 20]]), 'scan-crc32:aaaa0001');
    const decision = decideContinuationRegistration({ base, artifactB: null, extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(decision.kind).toBe('kept_base');
    expect(decision.kind === 'none' ? null : decision.artifact.geophone_positions).toEqual(base.geophone_positions);
  });

  it('ignores positions that cannot be moved (a transform that is not finite)', () => {
    const extension = remoteOnlyArtifact(block([[3, 1000, -1000, 20]]));
    const decision = decideContinuationRegistration({
      base: null, artifactB: extension, extensionToBase: { ...T_BA, tx: Number.NaN }, mergedScanFrameId: MERGED });
    expect(decision.kind).toBe('none');
  });

  it('leaves a registration without positions exactly as it was', () => {
    const base = { ...remoteOnlyArtifact(undefined, 'scan-crc32:aaaa0001') };
    const kept = decideContinuationRegistration({ base, artifactB: remoteOnlyArtifact(undefined), extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(kept.kind).toBe('kept_base');
    expect(kept.kind === 'none' ? null : 'geophone_positions' in kept.artifact).toBe(false);
    expect(kept.kind === 'none' ? null : kept.artifact).toEqual(bindRegistrationToScan(base, MERGED));
    expect(decideContinuationRegistration({ base: null, artifactB: remoteOnlyArtifact(undefined), extensionToBase: T_BA, mergedScanFrameId: MERGED }).kind).toBe('none');
  });
});

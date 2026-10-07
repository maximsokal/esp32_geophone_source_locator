import { describe, expect, it } from 'vitest';

import { defaultRegistrationTagSizes, toOccupancyRegistration, type RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import {
  bindRegistrationToScan,
  composeContinuationRegistration,
  compareRegistrations,
  decideContinuationRegistration,
  projectMatrixToLocalFloor,
} from '../../src/xr/registrationCompose';
import { localFloorMatrixToProject } from '../../src/xr/projectFrame';
import { webXrLocalFloorToProject } from '../../src/xr/scanViewportScene';

const MERGED = 'scan-crc32:00c0ffee';

type P3 = [number, number, number];

function apply(matrix: readonly number[], p: readonly number[]): P3 {
  return [
    matrix[0] * p[0] + matrix[1] * p[1] + matrix[2] * p[2] + matrix[3],
    matrix[4] * p[0] + matrix[5] * p[1] + matrix[6] * p[2] + matrix[7],
    matrix[8] * p[0] + matrix[9] * p[1] + matrix[10] * p[2] + matrix[11],
  ];
}

/** Local-floor (Y up) rotation about Y by yaw plus a translation with non-zero X, Y and Z. */
function localFloorMatrix(yaw: number, t: P3): number[] {
  const c = Math.cos(yaw), s = Math.sin(yaw);
  return [c, 0, s, t[0], 0, 1, 0, t[1], -s, 0, c, t[2], 0, 0, 0, 1];
}

function artifact(patch: Partial<RegistrationArtifact> = {}): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: null, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: 500, created_at_ms: 600, tag_size_m: defaultRegistrationTagSizes(),
    bindings: [{ tag_id: 1, node: 'G1', observation_count: 6, inlier_count: 6, position_scan_m: [0.4, 0.2, -0.7], spread_m: 0.002 }],
    sensor_to_scan: localFloorMatrix(0.6, [1.2, 0.3, -2.1]), mirrored: false, residual_rms_m: 0.01, threshold_m: 0.05,
    per_node: [{ node: 'G1', tagId: 1, residualM: 0.004, status: 'anchored' }], tier: 'minimum', valid: true, reason: null, ...patch,
  };
}

const T_BA = { yaw: -0.9, tx: 3.1, ty: -1.4, tz: 0.35 };

/** Applies the project-frame continuation transform T_BA (yaw about Z, then translation). */
function projectTba(p: readonly number[]): P3 {
  const c = Math.cos(T_BA.yaw), s = Math.sin(T_BA.yaw);
  return [c * p[0] - s * p[1] + T_BA.tx, s * p[0] + c * p[1] + T_BA.ty, p[2] + T_BA.tz];
}

/** Reference: project_in_A = T_BA * webXrLocalFloorToProject(R_B * p). */
function expectedProjectInA(rb: readonly number[], p: readonly number[]): P3 {
  const lf = apply(rb, p);
  return projectTba(webXrLocalFloorToProject(lf[0], lf[1], lf[2]));
}

describe('continuation registration composition (MAX-416)', () => {
  it('maps sensor points to the base frame exactly as T_BA * localFloorToProject(R_B * p) for a non-trivial basis case', () => {
    const b = artifact();
    const composed = composeContinuationRegistration(b, T_BA, MERGED)!;
    expect(composed.scan_frame_id).toBe(MERGED);
    expect(composed.frame).toBe('webxr_local_floor');
    expect(composed.threshold_m).toBe(b.threshold_m);
    expect(composed.residual_rms_m).toBe(b.residual_rms_m);
    expect(composed.geometry_computed_at_ms).toBe(b.geometry_computed_at_ms);
    const embedded = toOccupancyRegistration(composed)!;
    for (const p of [[0, 0, 0], [1, 0, 0], [0, 1.5, 0], [0, 0, 2], [0.7, -0.4, 1.3]]) {
      const viewer = apply(composed.sensor_to_scan!, p);
      const viewerProject = webXrLocalFloorToProject(viewer[0], viewer[1], viewer[2]);
      const expected = expectedProjectInA(b.sensor_to_scan!, p);
      viewerProject.forEach((value, axis) => expect(value).toBeCloseTo(expected[axis], 9));
      apply(embedded.sensor_to_scan, p).forEach((value, axis) => expect(value).toBeCloseTo(expected[axis], 9));
    }
    // The old behaviour copied R_B unchanged, which differs from the composition by T_BA.
    const old = apply(localFloorMatrixToProject(b.sensor_to_scan!), [1, 0, 0]);
    const wanted = expectedProjectInA(b.sensor_to_scan!, [1, 0, 0]);
    expect(Math.hypot(old[0] - wanted[0], old[1] - wanted[1], old[2] - wanted[2])).toBeGreaterThan(1);
  });

  it('moves tag binding positions to the base frame and inverts the basis change exactly', () => {
    const b = artifact();
    const composed = composeContinuationRegistration(b, T_BA, MERGED)!;
    const moved = composed.bindings[0].position_scan_m;
    const original = b.bindings[0].position_scan_m;
    const expected = projectTba(webXrLocalFloorToProject(original[0], original[1], original[2]));
    webXrLocalFloorToProject(moved[0], moved[1], moved[2]).forEach((value, axis) => expect(value).toBeCloseTo(expected[axis], 9));
    const roundTrip = projectMatrixToLocalFloor(localFloorMatrixToProject(b.sensor_to_scan!));
    roundTrip.forEach((value, index) => expect(value).toBeCloseTo(b.sensor_to_scan![index], 12));
  });

  it('returns null for an invalid tag result and never composes a non-finite transform', () => {
    expect(composeContinuationRegistration(artifact({ valid: false, sensor_to_scan: null, tier: null, reason: 'missing_main' }), T_BA, MERGED)).toBeNull();
    expect(composeContinuationRegistration(artifact(), { ...T_BA, tx: Number.NaN }, MERGED)).toBeNull();
    expect(composeContinuationRegistration(null, T_BA, MERGED)).toBeNull();
  });

  it('persists the composed registration when there is no base registration', () => {
    const decision = decideContinuationRegistration({ base: null, artifactB: artifact(), extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(decision.kind).toBe('composed');
    if (decision.kind === 'composed') expect(decision.artifact.scan_frame_id).toBe(MERGED);
    expect(decideContinuationRegistration({ base: null, artifactB: null, extensionToBase: T_BA, mergedScanFrameId: MERGED }).kind).toBe('none');
  });

  it('keeps the base registration, re-stamped, when the extension saw no valid tags', () => {
    const base = artifact({ scan_frame_id: 'scan-crc32:11111111' });
    const decision = decideContinuationRegistration({ base, artifactB: null, extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(decision.kind).toBe('kept_base');
    if (decision.kind === 'kept_base') {
      expect(decision.artifact.scan_frame_id).toBe(MERGED);
      expect(decision.artifact.sensor_to_scan).toEqual(base.sensor_to_scan);
    }
  });

  it('keeps the base within tolerance and reports a conflict (still keeping the base) beyond it', () => {
    const b = artifact();
    const exact = composeContinuationRegistration(b, T_BA, MERGED)!;
    const shifted = (dx: number) => bindRegistrationToScan({ ...exact, sensor_to_scan: exact.sensor_to_scan!.map((v, i) => (i === 3 ? v + dx : v)) }, 'scan-crc32:11111111');
    expect(decideContinuationRegistration({ base: shifted(0.05), artifactB: b, extensionToBase: T_BA, mergedScanFrameId: MERGED }).kind).toBe('kept_base');
    const far = shifted(0.25);
    const conflict = decideContinuationRegistration({ base: far, artifactB: b, extensionToBase: T_BA, mergedScanFrameId: MERGED });
    expect(conflict.kind).toBe('conflict');
    if (conflict.kind === 'conflict') {
      expect(conflict.artifact.sensor_to_scan).toEqual(far.sensor_to_scan);
      expect(conflict.agreement.translationDeltaM).toBeCloseTo(0.25, 9);
    }
  });

  it('measures rotation disagreement in degrees against the 5 degree tolerance', () => {
    const base = artifact({ sensor_to_scan: localFloorMatrix(0.6, [0, 0, 0]) });
    const near = artifact({ sensor_to_scan: localFloorMatrix(0.6 + (4 * Math.PI) / 180, [0, 0, 0]) });
    const far = artifact({ sensor_to_scan: localFloorMatrix(0.6 + (7 * Math.PI) / 180, [0, 0, 0]) });
    expect(compareRegistrations(near, base).yawDeltaDeg).toBeCloseTo(4, 6);
    expect(compareRegistrations(near, base).withinTolerance).toBe(true);
    expect(compareRegistrations(far, base).yawDeltaDeg).toBeCloseTo(7, 6);
    expect(compareRegistrations(far, base).withinTolerance).toBe(false);
  });
});

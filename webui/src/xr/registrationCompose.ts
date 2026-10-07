import type { RegistrationArtifact } from './fiducial/registrationArtifact';
import type { RigidTransform } from './scanContinuation';

/*
 * MAX-416: composing a Continue registration into the base scan frame.
 *
 * registration-v1.json stores sensor -> WebXR local-floor (Y up). A continuation extends the base
 * scan A with a NEW WebXR session whose frame B has its own local-floor, so tags seen during the
 * extension give R_B (sensor -> local-floor B). registerAndMergeScanContinuation returns the
 * accepted PROJECT-frame (Z up) transform T_BA (B -> A). With C the local-floor -> project change
 * of basis ([x,y,z] -> [x,-z,y]):
 *
 *     M_project_A = T_BA * C * M_lf_B      and      M_lf_A = C^-1 * T_BA * C * M_lf_B
 *
 * C^-1 is written here (and not in projectFrame.ts) because the shared frame helper is owned by
 * another change and only provides C; nothing else is duplicated.
 */

type Matrix4 = readonly number[];

/** local-floor -> project: rows r0 = x, r1 = -z, r2 = y. */
const LOCAL_FLOOR_TO_PROJECT: Matrix4 = [1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1];
/** project -> local-floor, the inverse of the above: rows r0 = x, r1 = z, r2 = -y. */
const PROJECT_TO_LOCAL_FLOOR: Matrix4 = [1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1];

/** A continuation registration may drift this far from an existing base registration and still be accepted. */
export const CONTINUATION_REGISTRATION_TOLERANCE_TRANSLATION_M = 0.10;
export const CONTINUATION_REGISTRATION_TOLERANCE_ROTATION_DEG = 5;

function multiply(a: Matrix4, b: Matrix4): number[] {
  const result = new Array<number>(16).fill(0);
  for (let row = 0; row < 4; row += 1) {
    for (let column = 0; column < 4; column += 1) {
      let sum = 0;
      for (let k = 0; k < 4; k += 1) sum += a[row * 4 + k] * b[k * 4 + column];
      result[row * 4 + column] = sum + 0; // adding zero keeps negative zero out of the artifact bytes
    }
  }
  return result;
}

/** Inverse of localFloorMatrixToProject: converts a project-frame matrix back to local-floor. */
export function projectMatrixToLocalFloor(matrix: readonly number[]): number[] {
  if (matrix.length !== 16 || matrix.some((value) => !Number.isFinite(value))) {
    throw new Error('Registration matrix must contain 16 finite row-major values.');
  }
  return multiply(PROJECT_TO_LOCAL_FLOOR, matrix);
}

/** Row-major 4x4 of a project-frame yaw (about +Z) + translation transform. */
export function projectTransformMatrix(transform: RigidTransform): number[] {
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  return [
    c, -s, 0, transform.tx,
    s, c, 0, transform.ty,
    0, 0, 1, transform.tz,
    0, 0, 0, 1,
  ];
}

function transformFinite(transform: RigidTransform): boolean {
  return [transform.yaw, transform.tx, transform.ty, transform.tz].every(Number.isFinite);
}

/** Conjugates a local-floor-B quantity into local-floor A through T_BA: C^-1 * T_BA * C. */
function localFloorBToA(transform: RigidTransform): number[] {
  return multiply(PROJECT_TO_LOCAL_FLOOR, multiply(projectTransformMatrix(transform), LOCAL_FLOOR_TO_PROJECT));
}

/**
 * Re-expresses a registration made in continuation frame B in the base frame A of the merged scan.
 * Returns null when the B registration is not a valid transform (nothing to compose). The result
 * keeps the geometry generation, quality and tag evidence of B; matrix and tag positions move to A.
 */
export function composeContinuationRegistration(
  artifactB: RegistrationArtifact | null,
  extensionToBase: RigidTransform,
  scanFrameId: string | null = null,
): RegistrationArtifact | null {
  if (artifactB === null || !artifactB.valid || artifactB.sensor_to_scan === null || artifactB.sensor_to_scan.length !== 16 ||
      !artifactB.sensor_to_scan.every(Number.isFinite) || !transformFinite(extensionToBase)) return null;
  const conjugated = localFloorBToA(extensionToBase);
  const sensorToScan = multiply(conjugated, artifactB.sensor_to_scan);
  sensorToScan[12] = 0; sensorToScan[13] = 0; sensorToScan[14] = 0; sensorToScan[15] = 1;
  return {
    ...artifactB,
    scan_frame_id: scanFrameId,
    sensor_to_scan: sensorToScan,
    bindings: artifactB.bindings.map((binding) => {
      const p = binding.position_scan_m;
      return {
        ...binding,
        position_scan_m: [
          conjugated[0] * p[0] + conjugated[1] * p[1] + conjugated[2] * p[2] + conjugated[3] + 0,
          conjugated[4] * p[0] + conjugated[5] * p[1] + conjugated[6] * p[2] + conjugated[7] + 0,
          conjugated[8] * p[0] + conjugated[9] * p[1] + conjugated[10] * p[2] + conjugated[11] + 0,
        ] as [number, number, number],
      };
    }),
  };
}

/** Re-stamps a registration with a (new) scan identity without touching any evidence. */
export function bindRegistrationToScan(artifact: RegistrationArtifact, scanFrameId: string | null): RegistrationArtifact {
  return { ...artifact, scan_frame_id: scanFrameId };
}

export interface RegistrationAgreement {
  translationDeltaM: number;
  /** Angle of the relative rotation (a gravity-aligned registration differs mostly in yaw). */
  yawDeltaDeg: number;
  residualDeltaM: number;
  withinTolerance: boolean;
}

function determinant3(m: readonly number[]): number {
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

/** Compares a newly composed registration against the existing base registration. */
export function compareRegistrations(composed: RegistrationArtifact, base: RegistrationArtifact): RegistrationAgreement {
  const a = composed.sensor_to_scan;
  const b = base.sensor_to_scan;
  if (a === null || b === null || a.length !== 16 || b.length !== 16) {
    return { translationDeltaM: Number.POSITIVE_INFINITY, yawDeltaDeg: Number.POSITIVE_INFINITY, residualDeltaM: Number.POSITIVE_INFINITY, withinTolerance: false };
  }
  const translationDeltaM = Math.hypot(a[3] - b[3], a[7] - b[7], a[11] - b[11]);
  let yawDeltaDeg = Number.POSITIVE_INFINITY;
  if ((determinant3(a) < 0) === (determinant3(b) < 0)) {
    // trace(R_base^T * R_composed) = 1 + 2 cos(angle)
    let trace = 0;
    for (let row = 0; row < 3; row += 1) for (let column = 0; column < 3; column += 1) trace += b[row * 4 + column] * a[row * 4 + column];
    const cosine = Math.max(-1, Math.min(1, (trace - 1) / 2));
    yawDeltaDeg = Math.acos(cosine) * 180 / Math.PI;
  }
  const residualDeltaM = composed.residual_rms_m - base.residual_rms_m;
  return {
    translationDeltaM,
    yawDeltaDeg,
    residualDeltaM,
    withinTolerance: translationDeltaM <= CONTINUATION_REGISTRATION_TOLERANCE_TRANSLATION_M &&
      yawDeltaDeg <= CONTINUATION_REGISTRATION_TOLERANCE_ROTATION_DEG,
  };
}

export type ContinuationRegistrationDecision =
  /** Nothing to persist: no applicable base registration and no valid tag result in the extension. */
  | { kind: 'none' }
  /** No base registration: the registration of the extension, composed into frame A, is the new one. */
  | { kind: 'composed'; artifact: RegistrationArtifact }
  /** Base registration kept (re-stamped); the extension had no valid tags or agreed within tolerance. */
  | { kind: 'kept_base'; artifact: RegistrationArtifact; agreement: RegistrationAgreement | null }
  /** Base registration kept because the composed one disagrees beyond tolerance (surface registration_conflict). */
  | { kind: 'conflict'; artifact: RegistrationArtifact; agreement: RegistrationAgreement };

/**
 * Chooses the registration for the merged scan. A high-quality existing base registration is never
 * silently replaced: it is kept (stamped with the merged scan identity) and a disagreement is reported.
 * The base must already have passed the shared applicability predicate against the base scan.
 */
export function decideContinuationRegistration(input: {
  base: RegistrationArtifact | null;
  artifactB: RegistrationArtifact | null;
  extensionToBase: RigidTransform;
  mergedScanFrameId: string;
}): ContinuationRegistrationDecision {
  const composed = composeContinuationRegistration(input.artifactB, input.extensionToBase, input.mergedScanFrameId);
  if (input.base === null) return composed === null ? { kind: 'none' } : { kind: 'composed', artifact: composed };
  const kept = bindRegistrationToScan(input.base, input.mergedScanFrameId);
  if (composed === null) return { kind: 'kept_base', artifact: kept, agreement: null };
  const agreement = compareRegistrations(composed, input.base);
  return agreement.withinTolerance
    ? { kind: 'kept_base', artifact: kept, agreement }
    : { kind: 'conflict', artifact: kept, agreement };
}

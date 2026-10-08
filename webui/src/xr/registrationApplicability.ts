import type { I18nKey } from '../i18n';
import type { RegistrationArtifact } from './fiducial/registrationArtifact';

/*
 * MAX-405: a registration is versioned, frame-bound evidence. It is applicable only to the exact
 * (geometry generation, scan frame generation) it was computed for. Object and session equality
 * stays the caller's duty. Every consumer -- tag restore, parent authority, occupancy/preview
 * builders, sensor placement, overlays -- shares this one predicate so that none of them can
 * disagree about which scan a registration belongs to.
 */
export type RegistrationInapplicableReason =
  | 'missing' | 'invalid' | 'reflected_transform' | 'quality' | 'geometry_mismatch' | 'scan_unbound' | 'scan_mismatch' | 'scan_unknown';
export type RegistrationApplicability = { ok: true } | { ok: false; reason: RegistrationInapplicableReason };

export type RegistrationTransformIssue = 'invalid' | 'reflected_transform' | null;

/** Uses the firmware registration tolerances for a row-major sensor-to-scan matrix. */
export function registrationTransformIssue(value: unknown): RegistrationTransformIssue {
  if (!Array.isArray(value) || value.length !== 16 || value.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) return 'invalid';
  const matrix = value as number[];
  if (matrix[12] !== 0 || matrix[13] !== 0 || matrix[14] !== 0 || matrix[15] !== 1) return 'invalid';
  const rotation = [matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6], matrix[8], matrix[9], matrix[10]];
  for (let row = 0; row < 3; row += 1) for (let other = 0; other < 3; other += 1) {
    const dot = rotation[row * 3] * rotation[other * 3] + rotation[row * 3 + 1] * rotation[other * 3 + 1] + rotation[row * 3 + 2] * rotation[other * 3 + 2];
    if (Math.abs(dot - (row === other ? 1 : 0)) > 1e-3) return 'invalid';
  }
  const determinant = rotation[0] * (rotation[4] * rotation[8] - rotation[5] * rotation[7]) -
    rotation[1] * (rotation[3] * rotation[8] - rotation[5] * rotation[6]) +
    rotation[2] * (rotation[3] * rotation[7] - rotation[4] * rotation[6]);
  if (Math.abs(determinant - 1) <= 1e-3) return null;
  return determinant < 0 && Math.abs(determinant + 1) <= 1e-3 ? 'reflected_transform' : 'invalid';
}

export interface RegistrationExpectation {
  /** Maximum computed_at_ms of the frozen localization geometry; null when unknown (not checked). */
  geometryComputedAtMs: number | null;
  /** Identity (`scan-crc32:xxxxxxxx`) of the scan the consumer is about to use; null when unknown. */
  scanFrameId: string | null;
}

/** Residual/threshold comparison slack, identical to the artifact decoder's quality invariant. */
export const REGISTRATION_QUALITY_EPSILON_M = 1e-9;

export function registrationQualityOk(artifact: Pick<RegistrationArtifact, 'residual_rms_m' | 'threshold_m'>): boolean {
  return Number.isFinite(artifact.threshold_m) && artifact.threshold_m > 0 &&
    Number.isFinite(artifact.residual_rms_m) && artifact.residual_rms_m >= 0 &&
    artifact.residual_rms_m <= artifact.threshold_m + REGISTRATION_QUALITY_EPSILON_M;
}

/** Frame binding only: geometry generation and scan identity (used for tag-only evidence too). */
export function registrationBindingApplicability(
  artifact: RegistrationArtifact | null,
  expected: RegistrationExpectation,
): RegistrationApplicability {
  if (artifact === null || artifact === undefined) return { ok: false, reason: 'missing' };
  if (expected.geometryComputedAtMs !== null && artifact.geometry_computed_at_ms !== expected.geometryComputedAtMs) {
    return { ok: false, reason: 'geometry_mismatch' };
  }
  const bound = artifact.scan_frame_id ?? null;
  if (bound === null) return { ok: false, reason: 'scan_unbound' };
  if (expected.scanFrameId === null) return { ok: false, reason: 'scan_unknown' };
  if (bound !== expected.scanFrameId) return { ok: false, reason: 'scan_mismatch' };
  return { ok: true };
}

/** Whether `artifact` may be applied to the scan/geometry described by `expected`. Fails closed. */
export function registrationApplicability(
  artifact: RegistrationArtifact | null,
  expected: RegistrationExpectation,
): RegistrationApplicability {
  if (artifact === null || artifact === undefined) return { ok: false, reason: 'missing' };
  const transformIssue = registrationTransformIssue(artifact.sensor_to_scan);
  if (transformIssue === 'reflected_transform') return { ok: false, reason: 'reflected_transform' };
  if (artifact.valid !== true || transformIssue !== null) return { ok: false, reason: 'invalid' };
  if (!registrationQualityOk(artifact)) return { ok: false, reason: 'quality' };
  return registrationBindingApplicability(artifact, expected);
}

export const REGISTRATION_INAPPLICABLE_KEYS: Readonly<Record<RegistrationInapplicableReason, I18nKey>> = {
  missing: 'xr.tag.registration.reason.missing',
  invalid: 'xr.tag.registration.reason.invalid',
  reflected_transform: 'xr.tag.registration.reason.invalid',
  quality: 'xr.tag.registration.reason.quality',
  geometry_mismatch: 'xr.tag.registration.reason.geometry_mismatch',
  scan_unbound: 'xr.tag.registration.reason.scan_unbound',
  scan_mismatch: 'xr.tag.registration.reason.scan_mismatch',
  scan_unknown: 'xr.tag.registration.reason.scan_unknown',
};

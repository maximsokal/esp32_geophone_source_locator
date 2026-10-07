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
  | 'missing' | 'invalid' | 'quality' | 'geometry_mismatch' | 'scan_unbound' | 'scan_mismatch' | 'scan_unknown';
export type RegistrationApplicability = { ok: true } | { ok: false; reason: RegistrationInapplicableReason };

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

function usableMatrix(artifact: RegistrationArtifact): boolean {
  const matrix = artifact.sensor_to_scan;
  return artifact.valid === true && Array.isArray(matrix) && matrix.length === 16 &&
    matrix.every((value) => typeof value === 'number' && Number.isFinite(value));
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
  if (!usableMatrix(artifact)) return { ok: false, reason: 'invalid' };
  if (!registrationQualityOk(artifact)) return { ok: false, reason: 'quality' };
  return registrationBindingApplicability(artifact, expected);
}

export const REGISTRATION_INAPPLICABLE_KEYS: Readonly<Record<RegistrationInapplicableReason, I18nKey>> = {
  missing: 'xr.tag.registration.reason.missing',
  invalid: 'xr.tag.registration.reason.invalid',
  quality: 'xr.tag.registration.reason.quality',
  geometry_mismatch: 'xr.tag.registration.reason.geometry_mismatch',
  scan_unbound: 'xr.tag.registration.reason.scan_unbound',
  scan_mismatch: 'xr.tag.registration.reason.scan_mismatch',
  scan_unknown: 'xr.tag.registration.reason.scan_unknown',
};

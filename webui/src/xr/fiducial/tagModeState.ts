import type { TagObservationFusion } from './tagObservationFusion';
import type { RegistrationArtifact } from './registrationArtifact';
import type { SensorToScanRegistration } from './rigidRegistration';
import { formatErrorText, translate, type I18nKey, type I18nLocale } from '../../i18n';
import { formatNumber } from '../../i18n/format';

/**
 * MAX-403: upper bound for tag finalization after an explicit Finish. A pending detector task gets
 * this long to settle; then its worker is disposed and the late result is ignored. The scan itself
 * is handed over for persistence immediately and never waits for this window.
 */
export const TAG_FINALIZE_WINDOW_MS = 3000;

export type TagChipStatus = { tagId: 0 | 1 | 2 | 3 | 4; node: string; text: string; status: 'not-seen' | 'seen' | 'confirmed' };
const TAG_NAMES = ['MAIN', 'G1', 'G2', 'G3', 'G4'] as const;
const REGISTRATION_REASON_KEYS: Readonly<Record<string, I18nKey>> = {
  no_geometry: 'xr.tag.reason.no_geometry',
  missing_main: 'xr.tag.reason.missing_main',
  too_few_remotes: 'xr.tag.reason.too_few_remotes',
  collinear: 'xr.tag.reason.collinear',
  gravity_inconsistent: 'xr.tag.reason.gravity_inconsistent',
  residual_too_high: 'xr.tag.reason.residual_too_high',
};

export function tagChipStatuses(fusion: TagObservationFusion, locale: I18nLocale = 'en'): TagChipStatus[] {
  return TAG_NAMES.map((label, tagId) => {
    const tag = fusion.byTagId[tagId as 0 | 1 | 2 | 3 | 4];
    if (tag === undefined) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, text: translate(locale, 'xr.tag.chip.not_seen', { node: label }), status: 'not-seen' };
    if (tag.confirmed) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'confirmed', text: translate(locale, 'xr.tag.chip.confirmed', { node: label }) };
    return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'seen', text: translate(locale, 'xr.tag.chip.seen', { node: label, inliers: formatNumber(locale, tag.inlierCount), minimum: formatNumber(locale, 5) }) };
  });
}

export function registrationOutcome(result: Pick<SensorToScanRegistration, 'valid' | 'tier' | 'reason'> | null, locale: I18nLocale = 'en'): string {
  if (result === null) return translate(locale, 'xr.tag.registration.none');
  if (result.valid) {
    const tierKey = result.tier === 'full' ? 'xr.tag.tier.full' : result.tier === 'minimum' ? 'xr.tag.tier.minimum' : 'xr.tag.tier.unknown';
    return translate(locale, 'xr.tag.registration.valid', { tier: translate(locale, tierKey) });
  }
  const reason = result.reason ?? 'unknown';
  const reasonKey = REGISTRATION_REASON_KEYS[reason];
  return translate(locale, 'xr.tag.registration.failed', { reason: reasonKey === undefined ? reason : translate(locale, reasonKey) });
}

const TAG_ERROR_KEYS: Readonly<Record<string, I18nKey>> = {
  'Open a recording before using tag registration.': 'xr.tag.open_recording',
  'Frozen localization geometry is unavailable for this recording.': 'xr.tag.geometry_unavailable',
  'AprilTag detector worker could not start.': 'xr.tag.error.detector_unavailable',
  'Frozen session geometry is unavailable; tag observations were not registered.': 'xr.tag.error.geometry_unavailable',
  'Registration artifact was not accepted.': 'xr.tag.error.not_accepted',
};
const TAG_ERROR_PREFIXES: ReadonlyArray<readonly [string, I18nKey]> = [
  ['Saved anchor status unavailable: ', 'xr.tag.error.saved_anchor_status'],
  ['Frozen session geometry unavailable: ', 'xr.tag.error.load_geometry'],
  ['Registration result is in memory but could not be saved: ', 'xr.tag.error.save_registration'],
  ['AprilTag detection failed: ', 'xr.tag.error.detect'],
];

export function tagModeErrorLabel(message: string, locale: I18nLocale): string {
  const key = TAG_ERROR_KEYS[message];
  if (key !== undefined) return translate(locale, key);
  const prefix = TAG_ERROR_PREFIXES.find(([text]) => message.startsWith(text));
  return prefix === undefined
    ? formatErrorText(locale, message)
    : translate(locale, prefix[1], { reason: tagModeErrorLabel(message.slice(prefix[0].length), locale) });
}

export function registrationArtifactFromResult(
  result: SensorToScanRegistration,
  fusion: TagObservationFusion,
  tagSizeM: Record<'0' | '1' | '2' | '3' | '4', number>,
  geometryComputedAtMs: number,
  createdAtMs: number,
  scanFrameId: string | null = null,
): RegistrationArtifact {
  const bindings = Object.values(fusion.byTagId).filter((tag) => tag !== undefined).map((tag) => ({
    tag_id: tag.tagId,
    node: TAG_NAMES[tag.tagId],
    observation_count: tag.observationCount,
    inlier_count: tag.inlierCount,
    position_scan_m: [...tag.positionScan] as [number, number, number],
    spread_m: tag.spreadM,
  }));
  return {
    schema: 1,
    frame: 'webxr_local_floor',
    scan_frame_id: scanFrameId,
    source: 'apriltag',
    family: 'tag36h11',
    geometry_computed_at_ms: geometryComputedAtMs,
    created_at_ms: createdAtMs,
    tag_size_m: tagSizeM,
    bindings,
    sensor_to_scan: result.sensorToScan,
    mirrored: result.mirrored,
    residual_rms_m: result.rmsResidualM,
    threshold_m: result.thresholdM,
    per_node: result.perNode,
    tier: result.tier,
    valid: result.valid,
    reason: result.reason,
  };
}

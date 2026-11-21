import type { TagObservationFusion } from './tagObservationFusion';
import type { RegistrationArtifact, RegistrationGeophonePositions } from './registrationArtifact';
import type { SensorToScanRegistration } from './rigidRegistration';
import { formatErrorText, translate, type I18nKey, type I18nLocale } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import { TAG_BINDINGS, tagNodeForId, type TagBinding } from './tagBindings.gen';

/**
 * MAX-403: upper bound for tag finalization after an explicit Finish. A pending detector task gets
 * this long to settle; then its worker is disposed and the late result is ignored. The scan itself
 * is handed over for persistence immediately and never waits for this window.
 */
export const TAG_FINALIZE_WINDOW_MS = 3000;

export type TagChipStatus = { tagId: 0 | 1 | 2 | 3 | 4; node: string; text: string; status: 'not-seen' | 'seen' | 'confirmed' | 'frame-jump' };
const REGISTRATION_REASON_KEYS: Readonly<Record<string, I18nKey>> = {
  no_geometry: 'xr.tag.reason.no_geometry',
  missing_main: 'xr.tag.reason.missing_main',
  too_few_remotes: 'xr.tag.reason.too_few_remotes',
  collinear: 'xr.tag.reason.collinear',
  gravity_inconsistent: 'xr.tag.reason.gravity_inconsistent',
  residual_too_high: 'xr.tag.reason.residual_too_high',
};

export function tagChipStatuses(fusion: TagObservationFusion, locale: I18nLocale = 'en', bindings: readonly TagBinding[] = TAG_BINDINGS): TagChipStatus[] {
  return bindings.map(({ tag_id: tagId, node: label }) => {
    const tag = fusion.byTagId[tagId];
    if (tag === undefined) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, text: translate(locale, 'xr.tag.chip.not_seen', { node: label }), status: 'not-seen' };
    if (tag.frameJump) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'frame-jump', text: translate(locale, 'xr.tag.chip.frame_jump', { node: label }) };
    if (tag.confirmed) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'confirmed', text: translate(locale, 'xr.tag.chip.confirmed', { node: label }) };
    return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'seen', text: translate(locale, 'xr.tag.chip.views_seen', { node: label, views: formatNumber(locale, tag.independentViewCount), minimum: formatNumber(locale, 3) }) };
  });
}

/** A device whose tag has just been confirmed by the live scan, and the devices it can now be paired with. */
export interface TagScanNotice {
  tagId: number;
  node: string;
  /** 'jump': the tag's sightings split into two groups (the scan frame shifted); `separationM` is the gap. Absent = a confirmation notice. */
  kind?: 'jump';
  separationM?: number;
  /** Other confirmed devices, in tag order; the distances to them are recalculated from the scan. */
  others: string[];
}

export function confirmedTagIds(fusion: TagObservationFusion): number[] {
  return Object.values(fusion.byTagId)
    .filter((tag) => tag !== undefined && tag.confirmed)
    .map((tag) => tag!.tagId)
    .sort((a, b) => a - b);
}

/** One notice per tag that became confirmed since `previousConfirmed`, in tag order. */
export function newTagScanNotices(
  previousConfirmed: readonly number[],
  fusion: TagObservationFusion,
  bindings: readonly TagBinding[] = TAG_BINDINGS,
): TagScanNotice[] {
  const confirmed = confirmedTagIds(fusion);
  return confirmed.filter((tagId) => !previousConfirmed.includes(tagId)).map((tagId) => ({
    tagId,
    node: tagNodeForId(tagId, bindings),
    others: confirmed.filter((other) => other !== tagId).map((other) => tagNodeForId(other, bindings)),
  }));
}

export function frameJumpTagIds(fusion: TagObservationFusion): number[] {
  return Object.values(fusion.byTagId).filter((tag) => tag !== undefined && tag.frameJump).map((tag) => tag!.tagId).sort((a, b) => a - b);
}

export function newFrameJumpNotices(
  previousJumped: readonly number[],
  fusion: TagObservationFusion,
  bindings: readonly TagBinding[] = TAG_BINDINGS,
): TagScanNotice[] {
  return frameJumpTagIds(fusion).filter((tagId) => !previousJumped.includes(tagId)).map((tagId) => ({
    tagId, node: tagNodeForId(tagId, bindings), others: [], kind: 'jump' as const, separationM: fusion.byTagId[tagId as 0 | 1 | 2 | 3 | 4]!.jumpSeparationM ?? 0,
  }));
}

export function tagScanNoticeText(notice: TagScanNotice, locale: I18nLocale = 'en'): string {
  if (notice.kind === 'jump') return translate(locale, 'xr.tag.notice.frame_jump', { node: notice.node, meters: formatNumber(locale, Math.round((notice.separationM ?? 0) * 10) / 10) });
  if (notice.others.length === 0) return translate(locale, 'xr.tag.notice.first', { node: notice.node });
  return translate(locale, 'xr.tag.notice.recalculating', {
    node: notice.node,
    pairs: notice.others.map((other) => `${notice.node}–${other}`).join(', '),
  });
}

export function registrationOutcome(
  result: (Pick<SensorToScanRegistration, 'valid' | 'tier' | 'reason'> & { geophone_positions?: RegistrationGeophonePositions }) | null,
  locale: I18nLocale = 'en',
): string {
  if (result === null) return translate(locale, 'xr.tag.registration.none');
  /* REMOTE_ONLY: there is no MAIN tag to fit against, so the outcome is how many geophone tag centres were taken. */
  if (result.geophone_positions !== undefined) {
    const taken = new Set(result.geophone_positions.positions.map((position) => position.node_id));
    const missing = ([1, 2, 3, 4] as const).filter((id) => !taken.has(id)).map((id) => `G${id}`);
    return missing.length === 0
      ? translate(locale, 'xr.tag.geophones.complete', { count: taken.size })
      : translate(locale, 'xr.tag.geophones.partial', { count: taken.size, missing: missing.join(', ') });
  }
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
  'High-resolution tag capture requires non-blocking WebGL2 readback.': 'xr.tag.readback_unavailable',
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
  tagBindings: readonly TagBinding[] = TAG_BINDINGS,
  geophonePositions: RegistrationGeophonePositions | null = null,
): RegistrationArtifact {
  const bindings = Object.values(fusion.byTagId).filter((tag) => tag !== undefined).map((tag) => ({
    tag_id: tag.tagId,
    node: tagNodeForId(tag.tagId, tagBindings),
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
    ...(geophonePositions === null ? {} : { geophone_positions: geophonePositions }),
  };
}

import type { TagObservationFusion } from './tagObservationFusion';
import type { RegistrationArtifact } from './registrationArtifact';
import type { SensorToScanRegistration } from './rigidRegistration';

export type TagChipStatus = { tagId: 0 | 1 | 2 | 3 | 4; node: string; text: string; status: 'not-seen' | 'seen' | 'confirmed' };
const TAG_NAMES = ['MAIN', 'G1', 'G2', 'G3', 'G4'] as const;

export function tagChipStatuses(fusion: TagObservationFusion): TagChipStatus[] {
  return TAG_NAMES.map((label, tagId) => {
    const tag = fusion.byTagId[tagId as 0 | 1 | 2 | 3 | 4];
    if (tag === undefined) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, text: `${label} not seen`, status: 'not-seen' };
    if (tag.confirmed) return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'confirmed', text: `${label} confirmed` };
    return { tagId: tagId as 0 | 1 | 2 | 3 | 4, node: label, status: 'seen', text: `${label} seen ${tag.inlierCount}/5` };
  });
}

export function registrationOutcome(result: Pick<SensorToScanRegistration, 'valid' | 'tier' | 'reason'> | null): string {
  if (result === null) return 'No registration attempted.';
  return result.valid ? `Valid ${result.tier ?? 'unknown'} tier.` : `Registration failed: ${(result.reason ?? 'unknown').replaceAll('_', ' ')}.`;
}

export function registrationArtifactFromResult(
  result: SensorToScanRegistration,
  fusion: TagObservationFusion,
  tagSizeM: Record<'0' | '1' | '2' | '3' | '4', number>,
  geometryComputedAtMs: number,
  createdAtMs: number,
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

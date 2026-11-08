import type { FusedRegistrationTag, FusedTagsById } from './rigidRegistration';
import { TAG_BINDINGS, type TagBinding } from './tagBindings.gen';

/** Where a geophone position came from: the centre of its AprilTag, taken as the sensor position without any offset. */
export const GEOPHONE_POSITION_SOURCE = 'APRILTAG_CENTER_APPROX' as const;
export type GeophoneNode = 'G1' | 'G2' | 'G3' | 'G4';
export const GEOPHONE_NODES: readonly GeophoneNode[] = ['G1', 'G2', 'G3', 'G4'];

export interface GeophonePosition {
  node: GeophoneNode;
  tagId: number;
  /** Tag centre in the scan frame (WebXR local-floor, metres), used as the geophone position. */
  positionScan: [number, number, number];
  source: typeof GEOPHONE_POSITION_SOURCE;
  tagSizeMm: number;
  spreadM: number;
  observationCount: number;
  inlierCount: number;
  independentViewCount: number | null;
}

export type GeophoneRejectionReason = 'missing' | 'unconfirmed' | 'spread_too_high' | 'stale' | 'invalid_position' | 'scan_mismatch';
export interface GeophonePositionRejection { node: GeophoneNode; tagId: number; reason: GeophoneRejectionReason }

export interface GeophonePositionsResult {
  positions: GeophonePosition[];
  byNode: Partial<Record<GeophoneNode, GeophonePosition>>;
  rejected: GeophonePositionRejection[];
  /** True only when every expected geophone has an accepted position. */
  complete: boolean;
  /** The housing tag (tag 0), kept as a landmark; it is never a geophone and never needed. */
  housingLandmark: { tagId: number; positionScan: [number, number, number] } | null;
  scanFrameId: string | null;
}

export interface GeophonePositionsInput {
  tags: readonly FusedRegistrationTag[] | FusedTagsById;
  bindings?: readonly TagBinding[];
  expectedNodes?: readonly GeophoneNode[];
  /** Largest spread of a tag's inlier observations (metres) that still counts as a stable pose. */
  maxSpreadM?: number;
  /** When a tag was last seen (ms), by tag id; with nowMs and maxAgeMs a tag not seen recently is stale. */
  lastSeenAtMs?: Readonly<Partial<Record<number, number>>>;
  nowMs?: number;
  maxAgeMs?: number;
  /** The scan these tags were observed in and the scan the consumer is about to use; a mismatch rejects every position. */
  scanFrameId?: string | null;
  expectedScanFrameId?: string | null;
}

/** A tag pose whose inlier observations spread wider than this is not a stable centre. */
export const DEFAULT_MAX_TAG_SPREAD_M = 0.05;

function isFiniteVector(value: unknown): value is [number, number, number] {
  return Array.isArray(value) && value.length === 3 && value.every((entry) => typeof entry === 'number' && Number.isFinite(entry));
}

/**
 * The REMOTE_ONLY registration: the centre of each geophone's AprilTag is taken as that geophone's position in the scan
 * frame, with no offset (a design choice, the stick-on tags are not calibrated). Nothing is made up: a geophone whose tag is
 * missing, unconfirmed, unstable, old or from another scan gets no position and a reason, never zero or a previous value.
 * Tag 0 (the housing) is kept as a landmark only.
 */
export function geophonePositionsFromTags(input: GeophonePositionsInput): GeophonePositionsResult {
  const bindings = input.bindings ?? TAG_BINDINGS;
  const expectedNodes = input.expectedNodes ?? GEOPHONE_NODES;
  const maxSpreadM = input.maxSpreadM ?? DEFAULT_MAX_TAG_SPREAD_M;
  const scanFrameId = input.scanFrameId ?? null;
  const values: readonly (FusedRegistrationTag | undefined)[] = Array.isArray(input.tags) ? input.tags : Object.values(input.tags);
  const byTagId = new Map<number, FusedRegistrationTag>();
  for (const tag of values) if (tag !== undefined && Number.isInteger(tag.tagId) && !byTagId.has(tag.tagId)) byTagId.set(tag.tagId, tag);

  const scanMismatch = input.expectedScanFrameId !== undefined && scanFrameId !== input.expectedScanFrameId;
  const freshnessRequested = input.nowMs !== undefined && input.maxAgeMs !== undefined;
  const positions: GeophonePosition[] = [];
  const rejected: GeophonePositionRejection[] = [];
  const byNode: Partial<Record<GeophoneNode, GeophonePosition>> = {};

  for (const node of expectedNodes) {
    const binding = bindings.find((entry) => entry.node === node);
    const tagId = binding?.tag_id ?? -1;
    const reject = (reason: GeophoneRejectionReason): void => { rejected.push({ node, tagId, reason }); };
    const tag = byTagId.get(tagId);
    if (binding === undefined || tag === undefined) { reject('missing'); continue; }
    if (scanMismatch) { reject('scan_mismatch'); continue; }
    if (!isFiniteVector(tag.positionScan)) { reject('invalid_position'); continue; }
    if (!tag.confirmed) { reject('unconfirmed'); continue; }
    if (!Number.isFinite(tag.spreadM) || tag.spreadM > maxSpreadM) { reject('spread_too_high'); continue; }
    if (freshnessRequested) {
      const seen = input.lastSeenAtMs?.[tagId];
      if (seen === undefined || !Number.isFinite(seen) || input.nowMs! - seen > input.maxAgeMs! || seen > input.nowMs!) { reject('stale'); continue; }
    }
    const views = (tag as { independentViewCount?: unknown }).independentViewCount;
    const position: GeophonePosition = {
      node, tagId,
      positionScan: [tag.positionScan[0], tag.positionScan[1], tag.positionScan[2]],
      source: GEOPHONE_POSITION_SOURCE,
      tagSizeMm: binding.size_mm,
      spreadM: tag.spreadM,
      observationCount: tag.observationCount,
      inlierCount: tag.inlierCount,
      independentViewCount: typeof views === 'number' ? views : null,
    };
    positions.push(position);
    byNode[node] = position;
  }

  const housingBinding = bindings.find((entry) => entry.node === 'MAIN');
  const housing = housingBinding === undefined ? undefined : byTagId.get(housingBinding.tag_id);
  const housingLandmark = housing !== undefined && housing.confirmed && isFiniteVector(housing.positionScan)
    ? { tagId: housing.tagId, positionScan: [housing.positionScan[0], housing.positionScan[1], housing.positionScan[2]] as [number, number, number] }
    : null;
  return { positions, byNode, rejected, complete: rejected.length === 0 && positions.length === expectedNodes.length, housingLandmark, scanFrameId };
}

export interface BleDistanceInput { nodeA: number; nodeB: number; distanceMm: number; valid: boolean }
export interface PairResidual {
  nodeA: GeophoneNode;
  nodeB: GeophoneNode;
  tagDistanceMm: number;
  bleDistanceMm: number | null;
  /** Tag-centre distance minus the BLE distance; null when the BLE pair has no valid distance (never 0). */
  residualMm: number | null;
}

/**
 * Per pair of accepted geophone positions: the distance between the two tag centres, the BLE distance and their difference.
 * The difference is a report, not a correction: it may be large (Channel Sounding error, a tag that is not on the sensor,
 * scan error) and is never used to move a tag centre. A pair without a valid BLE distance has no residual (null, not 0).
 */
export function bleVsTagResiduals(positions: GeophonePositionsResult, ble: readonly BleDistanceInput[]): PairResidual[] {
  const nodeOf = (id: number): GeophoneNode | null => (Number.isInteger(id) && id >= 1 && id <= 4 ? GEOPHONE_NODES[id - 1] : null);
  const measured = new Map<string, number>();
  for (const entry of ble) {
    const a = nodeOf(entry.nodeA);
    const b = nodeOf(entry.nodeB);
    if (a === null || b === null || a === b || !entry.valid || !Number.isFinite(entry.distanceMm) || entry.distanceMm <= 0) continue;
    measured.set(a < b ? `${a}-${b}` : `${b}-${a}`, entry.distanceMm);
  }
  const residuals: PairResidual[] = [];
  for (let i = 0; i < GEOPHONE_NODES.length; i += 1) {
    for (let j = i + 1; j < GEOPHONE_NODES.length; j += 1) {
      const nodeA = GEOPHONE_NODES[i];
      const nodeB = GEOPHONE_NODES[j];
      const a = positions.byNode[nodeA];
      const b = positions.byNode[nodeB];
      if (a === undefined || b === undefined) continue;
      const tagDistanceMm = 1000 * Math.hypot(
        a.positionScan[0] - b.positionScan[0], a.positionScan[1] - b.positionScan[1], a.positionScan[2] - b.positionScan[2]);
      const bleDistanceMm = measured.get(`${nodeA}-${nodeB}`) ?? null;
      residuals.push({ nodeA, nodeB, tagDistanceMm, bleDistanceMm, residualMm: bleDistanceMm === null ? null : tagDistanceMm - bleDistanceMm });
    }
  }
  return residuals;
}

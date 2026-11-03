import type { LocalizationSessionGeometry } from '../api/deviceApi';
import type { RegistrationArtifact } from './fiducial/registrationArtifact';

/**
 * Distances between the devices recalculated from the 3D scan.
 *
 * Every device carries an AprilTag; the scan measures the tag positions in metric WebXR space, so the distance between two tags is
 * a metric distance between two devices. The Channel Sounding distance of the same pair (from the solved sensor geometry) is
 * compared with it.
 *
 * Error model, shown to the operator next to the numbers:
 *  - before = |scan distance - Channel Sounding distance|, the scan being the reference;
 *  - after  = the uncertainty of the scan distance: the tag position spreads of both devices plus a scale term
 *             (SCAN_SCALE_UNCERTAINTY_FRACTION of the distance);
 *  - the reduction is 1 - RMS(after) / RMS(before) over the recalculated pairs, never below 0.
 * The scan is taken as the reference: this is a claim about agreement with the scan, not an independent ground truth.
 */
export const SCAN_SCALE_UNCERTAINTY_FRACTION = 0.01;
/** Same count the live fusion needs before a tag counts as confirmed. */
export const SCAN_MIN_TAG_INLIERS = 5;
/** Pairs needed before the common offset is presented as a calibration value. */
export const SCAN_MIN_PAIRS_FOR_OFFSET = 3;

export type RefinementNode = 'MAIN' | 'G1' | 'G2' | 'G3' | 'G4';
const NODES: readonly RefinementNode[] = ['MAIN', 'G1', 'G2', 'G3', 'G4'];

export interface RefinedPair {
  a: RefinementNode;
  b: RefinementNode;
  csMm: number;
  scanMm: number;
  /** scan - cs, in mm: positive when the scan distance is longer. */
  correctionMm: number;
  beforeMm: number;
  afterMm: number;
  /** This pair alone, 0..100. Null when there was no disagreement to reduce. */
  reductionPercent: number | null;
}

export interface RefinementSummary {
  pairCount: number;
  rmsBeforeMm: number;
  rmsAfterMm: number;
  /** 0..100, null when the pairs agree exactly (nothing to reduce). */
  reductionPercent: number | null;
  /**
   * Median of (Channel Sounding - scan) over the pairs: the part of the error that is the same for every pair, i.e. what a per-board
   * offset calibration could remove. Positive = Channel Sounding reads long.
   */
  offsetMm: number;
  /** RMS of the pair errors left after that offset is removed: the part no constant offset explains. */
  spreadAfterOffsetMm: number;
  /** An offset from fewer than SCAN_MIN_PAIRS_FOR_OFFSET pairs says little; the card says so instead of presenting it as a calibration. */
  offsetReliable: boolean;
}

export type RefinementUnavailableReason = 'no_registration' | 'no_geometry' | 'too_few_devices' | 'no_common_pairs';

export type ScanDistanceRefinement =
  | { status: 'ok'; scannedNodes: RefinementNode[]; pairs: RefinedPair[]; skippedNodes: RefinementNode[]; summary: RefinementSummary }
  | { status: 'unavailable'; reason: RefinementUnavailableReason; scannedNodes: RefinementNode[] };

type Vec3 = readonly [number, number, number];

function nodeForId(nodeId: number | null | undefined): RefinementNode | null {
  return Number.isInteger(nodeId) && nodeId! >= 0 && nodeId! <= 4 ? NODES[nodeId!] : null;
}

/** Channel Sounding position of each node in millimetres in the sensor frame (MAIN is the origin). */
function csPositions(geometry: LocalizationSessionGeometry): Map<RefinementNode, Vec3> {
  const result = new Map<RefinementNode, Vec3>();
  let hasMain = false;
  for (const pod of geometry.pods) {
    const node = nodeForId(pod.node_id);
    if (node === null || pod.computed !== true || pod.assigned === false) continue;
    if (node === 'MAIN') {
      hasMain = true;
      result.set('MAIN', [0, 0, 0]);
      continue;
    }
    const x = pod.x_mm, y = pod.y_mm, z = pod.z_mm;
    if (x === undefined || y === undefined || z === undefined || !Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    if (result.has(node)) continue;
    result.set(node, [x, y, z]);
  }
  if (!hasMain && !result.has('MAIN') && geometry.pods.some((pod) => pod.computed === true)) result.set('MAIN', [0, 0, 0]);
  return result;
}

function distance(a: Vec3, b: Vec3): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

/** The devices the scan saw with enough observations, in node order. */
export function scannedNodes(registration: RegistrationArtifact | null): RefinementNode[] {
  if (registration === null) return [];
  const seen = new Set<RefinementNode>();
  for (const binding of registration.bindings) {
    if (binding.inlier_count >= SCAN_MIN_TAG_INLIERS && Number.isFinite(binding.spread_m) && binding.spread_m >= 0 &&
        binding.position_scan_m.length === 3 && binding.position_scan_m.every(Number.isFinite)) seen.add(binding.node);
  }
  return NODES.filter((node) => seen.has(node));
}

export function computeScanDistanceRefinement(
  geometry: LocalizationSessionGeometry | null,
  registration: RegistrationArtifact | null,
): ScanDistanceRefinement {
  const scanned = scannedNodes(registration);
  if (registration === null || scanned.length === 0) return { status: 'unavailable', reason: 'no_registration', scannedNodes: scanned };
  if (scanned.length < 2) return { status: 'unavailable', reason: 'too_few_devices', scannedNodes: scanned };
  if (geometry === null || !geometry.available) return { status: 'unavailable', reason: 'no_geometry', scannedNodes: scanned };

  const cs = csPositions(geometry);
  const bindingByNode = new Map(registration.bindings.map((binding) => [binding.node, binding]));
  const pairs: RefinedPair[] = [];
  const skipped = new Set<RefinementNode>();
  for (const node of scanned) if (!cs.has(node)) skipped.add(node);

  for (let i = 0; i < scanned.length; i += 1) {
    for (let j = i + 1; j < scanned.length; j += 1) {
      const a = scanned[i], b = scanned[j];
      const csA = cs.get(a), csB = cs.get(b);
      if (csA === undefined || csB === undefined) continue;
      const bindingA = bindingByNode.get(a)!, bindingB = bindingByNode.get(b)!;
      const csMm = distance(csA, csB);
      const scanMm = distance(bindingA.position_scan_m as Vec3, bindingB.position_scan_m as Vec3) * 1000;
      const beforeMm = Math.abs(scanMm - csMm);
      const afterMm = Math.sqrt(
        (bindingA.spread_m * 1000) ** 2 + (bindingB.spread_m * 1000) ** 2 + (SCAN_SCALE_UNCERTAINTY_FRACTION * scanMm) ** 2,
      );
      pairs.push({
        a, b, csMm, scanMm, correctionMm: scanMm - csMm, beforeMm, afterMm,
        reductionPercent: beforeMm > 0 ? Math.max(0, (1 - afterMm / beforeMm) * 100) : null,
      });
    }
  }
  if (pairs.length === 0) return { status: 'unavailable', reason: 'no_common_pairs', scannedNodes: scanned };

  const rms = (values: number[]): number => Math.sqrt(values.reduce((sum, value) => sum + value * value, 0) / values.length);
  const rmsBeforeMm = rms(pairs.map((pair) => pair.beforeMm));
  const rmsAfterMm = rms(pairs.map((pair) => pair.afterMm));
  const sorted = pairs.map((pair) => pair.csMm - pair.scanMm).sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  const offsetMm = sorted.length % 2 === 0 ? (sorted[middle - 1] + sorted[middle]) / 2 : sorted[middle];
  return {
    status: 'ok',
    scannedNodes: scanned,
    skippedNodes: [...skipped],
    pairs,
    summary: {
      pairCount: pairs.length,
      rmsBeforeMm,
      rmsAfterMm,
      reductionPercent: rmsBeforeMm > 0 ? Math.max(0, (1 - rmsAfterMm / rmsBeforeMm) * 100) : null,
      offsetMm,
      spreadAfterOffsetMm: rms(pairs.map((pair) => pair.csMm - pair.scanMm - offsetMm)),
      offsetReliable: pairs.length >= SCAN_MIN_PAIRS_FOR_OFFSET,
    },
  };
}

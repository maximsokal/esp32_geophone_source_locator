/**
 * Pure tag-observation fusion primitives. The capture pipeline must attach the
 * camera pose from the frame that captured the image; using a later XR pose
 * after asynchronous worker completion would assign false viewpoint evidence.
 * Legacy observations without pose provenance all count as one shared view.
 */
export const TAG_OBSERVATION_POSITION_BUCKET_METERS = 0.15;
export const TAG_OBSERVATION_DIRECTION_BUCKET_DEGREES = 15;
/**
 * Two sightings are independent views only if the camera really moved between them: the translation, not the heading, gives the
 * geometry that fixes a position. Turning the phone on the spot (or tilting it) adds no baseline (issue #17).
 */
export const TAG_OBSERVATION_MIN_BASELINE_METERS = 0.10;
/**
 * Sightings of one tag closer than this chain into one cluster. A tag does not move, so two clusters of at least
 * TAG_OBSERVATION_MIN_CLUSTER_SIZE sightings each, further apart than this, mean the scan frame shifted under the tag (tracking relocalised
 * without an XRReferenceSpace `reset` event, as in the 2026-11-21 phone run: the same tag moved 4.2 m between two groups of sightings).
 */
export const TAG_OBSERVATION_CLUSTER_LINK_METERS = 0.30;
export const TAG_OBSERVATION_MIN_CLUSTER_SIZE = 2;

export type TagObservationVector3 = readonly [number, number, number];

export interface TagObservation {
  tagId: number;
  positionScan: TagObservationVector3;
  capturedAtMs?: number;
  /** Camera position in the same frame as positionScan, sampled with the captured image. */
  cameraPosition?: TagObservationVector3;
  /** Camera forward direction in the same frame as positionScan, sampled with the captured image. */
  cameraForward?: TagObservationVector3;
  /** Larger finite values are preferred when repeated samples share a pose bucket. */
  quality?: number;
}

export interface FusedTagObservation {
  tagId: number;
  positionScan: [number, number, number];
  spreadM: number;
  observationCount: number;
  inlierCount: number;
  independentViewCount: number;
  confirmed: boolean;
  /** The sightings split into two or more separate clusters: the frame jumped. Never confirmed; `jumpSeparationM` is the gap between the two largest clusters. */
  frameJump: boolean;
  jumpSeparationM: number | null;
}

export interface TagObservationFusion {
  byTagId: Partial<Record<0 | 1 | 2 | 3 | 4, FusedTagObservation>>;
  /** Invalid IDs, non-finite positions, and robustly rejected outliers. */
  rejectedCount: number;
}

export interface TagObservationFusionOptions {
  minObservations?: number;
  minIndependentViews?: number;
}

function finiteVector3(value: unknown): value is TagObservationVector3 {
  return Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);
}

function normalizedForward(value: unknown): [number, number, number] | null {
  if (!finiteVector3(value)) return null;
  const length = Math.hypot(value[0], value[1], value[2]);
  if (!(length > 1e-9)) return null;
  return [value[0] / length, value[1] / length, value[2] / length];
}

/** Quantized camera viewpoint key; observations without a usable pose share one legacy bucket. */
export function tagObservationViewBucketKey(observation: TagObservation): string {
  if (!finiteVector3(observation.cameraPosition)) return 'legacy';
  const forward = normalizedForward(observation.cameraForward);
  if (forward === null) return 'legacy';

  const position = observation.cameraPosition.map((value) =>
    Math.floor(value / TAG_OBSERVATION_POSITION_BUCKET_METERS));
  /* WebXR local-floor: +Y is up and the camera looks along -Z. Azimuth turns about Y (from X and -Z), elevation is the Y component. */
  const yawDegrees = (Math.atan2(forward[0], -forward[2]) * 180 / Math.PI + 360) % 360;
  const yawBucketCount = 360 / TAG_OBSERVATION_DIRECTION_BUCKET_DEGREES;
  const yawBucket = Math.round(yawDegrees / TAG_OBSERVATION_DIRECTION_BUCKET_DEGREES) % yawBucketCount;
  const pitchDegrees = Math.asin(Math.max(-1, Math.min(1, forward[1]))) * 180 / Math.PI;
  const pitchBucket = Math.round(pitchDegrees / TAG_OBSERVATION_DIRECTION_BUCKET_DEGREES);
  return `${position[0]}:${position[1]}:${position[2]}|${yawBucket}:${pitchBucket}`;
}

/**
 * Independent views of one tag: sightings whose camera positions are pairwise at least the minimum baseline apart (greedy, in a fixed
 * order so the count does not depend on the order of arrival). Sightings without a camera pose all share one legacy view.
 */
export function countIndependentViews(observations: readonly TagObservation[]): number {
  const positions: TagObservationVector3[] = [];
  let legacy = false;
  for (const observation of observations) {
    if (finiteVector3(observation.cameraPosition)) positions.push(observation.cameraPosition);
    else legacy = true;
  }
  positions.sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2]);
  const accepted: TagObservationVector3[] = [];
  for (const position of positions) {
    const apart = accepted.every((other) => Math.hypot(position[0] - other[0], position[1] - other[1], position[2] - other[2]) >= TAG_OBSERVATION_MIN_BASELINE_METERS);
    if (apart) accepted.push(position);
  }
  return accepted.length + (legacy ? 1 : 0);
}

/** Single-linkage clusters of positions (link distance TAG_OBSERVATION_CLUSTER_LINK_METERS); each cluster is a list of indices, largest first. */
export function clusterTagPositions(positions: readonly TagObservationVector3[]): number[][] {
  const parent = positions.map((_, index) => index);
  const find = (start: number): number => {
    let root = start;
    while (parent[root] !== root) root = parent[root];
    let node = start;
    while (parent[node] !== root) { const next = parent[node]; parent[node] = root; node = next; }
    return root;
  };
  for (let i = 0; i < positions.length; i++) {
    for (let j = i + 1; j < positions.length; j++) {
      if (Math.hypot(positions[i][0] - positions[j][0], positions[i][1] - positions[j][1], positions[i][2] - positions[j][2]) <= TAG_OBSERVATION_CLUSTER_LINK_METERS) {
        parent[find(i)] = find(j);
      }
    }
  }
  const byRoot = new Map<number, number[]>();
  positions.forEach((_, index) => { const root = find(index); const list = byRoot.get(root); if (list === undefined) byRoot.set(root, [index]); else list.push(index); });
  return [...byRoot.values()].sort((a, b) => b.length - a.length || a[0] - b[0]);
}

function median(values: readonly number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ? (sorted[middle - 1] + sorted[middle]) / 2 : sorted[middle];
}

function threshold(value: number | undefined, fallback: number): number {
  return value === undefined || !Number.isFinite(value) ? fallback : Math.max(1, Math.floor(value));
}

/**
 * Pure deterministic fusion. Call with TagObservationStore.getObservations()
 * so exact median/MAD work stays within the store's fixed reservoir budget.
 */
export function fuseTagObservations(
  observations: readonly TagObservation[],
  options: TagObservationFusionOptions = {},
): TagObservationFusion {
  const minObservations = threshold(options.minObservations, 5);
  const minIndependentViews = threshold(options.minIndependentViews, 3);
  const groups: TagObservation[][] = [[], [], [], [], []];
  let rejectedCount = 0;
  for (const observation of observations) {
    if (!Number.isInteger(observation.tagId) || observation.tagId < 0 || observation.tagId > 4 ||
        !finiteVector3(observation.positionScan)) {
      rejectedCount += 1;
      continue;
    }
    groups[observation.tagId].push(observation);
  }

  const byTagId: TagObservationFusion['byTagId'] = {};
  groups.forEach((group, tagId) => {
    if (group.length === 0) return;
    const medianPosition: [number, number, number] = [0, 1, 2].map((axis) =>
      median(group.map((item) => item.positionScan[axis]))) as [number, number, number];
    const distances = group.map((item) => Math.hypot(
      item.positionScan[0] - medianPosition[0], item.positionScan[1] - medianPosition[1], item.positionScan[2] - medianPosition[2],
    ));
    /*
     * `distances` are already residuals about the coordinate median, so their robust scale is the median residual itself (a MAD about
     * ZERO). Taking the MAD around the median RADIUS instead gave 0 for any evenly scattered group and the 5 mm floor then dropped
     * every sample but one (issue #19). At least half of the samples are within the median residual, so the gate is never empty and
     * the estimate does not depend on observation order.
     */
    const gateM = Math.max(3 * 1.4826 * median(distances), 0.005);
    let inliers = group.filter((_, index) => distances[index] <= gateM);
    if (inliers.length === 0) inliers = [group[distances.indexOf(Math.min(...distances))]];
    rejectedCount += group.length - inliers.length;
    const positionScan: [number, number, number] = [0, 1, 2].map((axis) =>
      inliers.reduce((sum, item) => sum + item.positionScan[axis], 0) / inliers.length) as [number, number, number];
    const spreadM = Math.sqrt(inliers.reduce((sum, item) => sum +
      (item.positionScan[0] - positionScan[0]) ** 2 +
      (item.positionScan[1] - positionScan[1]) ** 2 +
      (item.positionScan[2] - positionScan[2]) ** 2, 0) / inliers.length);
    const independentViewCount = countIndependentViews(inliers);
    const substantial = clusterTagPositions(group.map((item) => item.positionScan)).filter((cluster) => cluster.length >= TAG_OBSERVATION_MIN_CLUSTER_SIZE);
    const frameJump = substantial.length >= 2;
    const centroidAxis = (cluster: readonly number[], axis: number): number =>
      cluster.reduce((sum, index) => sum + group[index].positionScan[axis], 0) / cluster.length;
    const jumpSeparationM = frameJump
      ? Math.hypot(centroidAxis(substantial[0], 0) - centroidAxis(substantial[1], 0), centroidAxis(substantial[0], 1) - centroidAxis(substantial[1], 1), centroidAxis(substantial[0], 2) - centroidAxis(substantial[1], 2))
      : null;
    byTagId[tagId as 0 | 1 | 2 | 3 | 4] = {
      tagId,
      positionScan,
      spreadM,
      observationCount: group.length,
      inlierCount: inliers.length,
      independentViewCount,
      confirmed: !frameJump && inliers.length >= minObservations && independentViewCount >= minIndependentViews,
      frameJump,
      jumpSeparationM,
    };
  });
  return { byTagId, rejectedCount };
}

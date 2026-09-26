export interface TagObservation {
  tagId: number;
  positionScan: readonly [number, number, number];
}

export interface FusedTagObservation {
  tagId: number;
  positionScan: [number, number, number];
  spreadM: number;
  observationCount: number;
  inlierCount: number;
  confirmed: boolean;
}

export interface TagObservationFusion {
  byTagId: Partial<Record<0 | 1 | 2 | 3 | 4, FusedTagObservation>>;
  /** Invalid IDs, non-finite positions, and robustly rejected outliers. */
  rejectedCount: number;
}

function median(values: readonly number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ? (sorted[middle - 1] + sorted[middle]) / 2 : sorted[middle];
}

/** Pure and deterministic tag-position fusion; no clock or session state is read. */
export function fuseTagObservations(
  observations: readonly TagObservation[],
  options: { minObservations?: number } = {},
): TagObservationFusion {
  const minObservations = Math.max(1, Math.floor(options.minObservations ?? 5));
  const groups: TagObservation[][] = [[], [], [], [], []];
  let rejectedCount = 0;
  for (const observation of observations) {
    if (!Number.isInteger(observation.tagId) || observation.tagId < 0 || observation.tagId > 4 ||
        !Array.isArray(observation.positionScan) || observation.positionScan.length !== 3 ||
        observation.positionScan.some((value) => !Number.isFinite(value))) {
      rejectedCount += 1;
      continue;
    }
    groups[observation.tagId].push(observation);
  }

  const byTagId: TagObservationFusion['byTagId'] = {};
  groups.forEach((group, tagId) => {
    if (group.length === 0) return;
    const medianPosition: [number, number, number] = [0, 1, 2].map((axis) => median(group.map((item) => item.positionScan[axis]))) as [number, number, number];
    const distances = group.map((item) => Math.hypot(
      item.positionScan[0] - medianPosition[0], item.positionScan[1] - medianPosition[1], item.positionScan[2] - medianPosition[2],
    ));
    const distanceMedian = median(distances);
    const mad = median(distances.map((distance) => Math.abs(distance - distanceMedian)));
    const gateM = Math.max(3 * 1.4826 * mad, 0.005);
    let inliers = group.filter((_, index) => distances[index] <= gateM);
    if (inliers.length === 0) inliers = [group[distances.indexOf(Math.min(...distances))]];
    rejectedCount += group.length - inliers.length;
    const positionScan: [number, number, number] = [0, 1, 2].map((axis) =>
      inliers.reduce((sum, item) => sum + item.positionScan[axis], 0) / inliers.length) as [number, number, number];
    const spreadM = Math.sqrt(inliers.reduce((sum, item) => sum +
      (item.positionScan[0] - positionScan[0]) ** 2 +
      (item.positionScan[1] - positionScan[1]) ** 2 +
      (item.positionScan[2] - positionScan[2]) ** 2, 0) / inliers.length);
    byTagId[tagId as 0 | 1 | 2 | 3 | 4] = {
      tagId,
      positionScan,
      spreadM,
      observationCount: group.length,
      inlierCount: inliers.length,
      confirmed: inliers.length >= minObservations,
    };
  });
  return { byTagId, rejectedCount };
}

import { symmetricEigen3 } from '../robustPlaneGeometry';

export type RegistrationNode = 'MAIN' | 'G1' | 'G2' | 'G3' | 'G4';
export type RegistrationTier = 'full' | 'minimum';
export type RegistrationNodeStatus = 'anchored' | 'visual_only' | 'provisional';
export type Vector3 = readonly [number, number, number];
export type Matrix3 = readonly [number, number, number, number, number, number, number, number, number];

export interface Svd3Result {
  /** Row-major orthogonal factors and descending non-negative singular values. */
  U: Matrix3;
  singularValues: [number, number, number];
  V: Matrix3;
}

/** Small real 3x3 SVD based on the project's symmetric Jacobi eigensolver. */
export function svd3(matrix: readonly number[]): Svd3Result {
  if (matrix.length !== 9 || matrix.some((value) => !Number.isFinite(value))) throw new Error('svd3 expects nine finite values.');
  const atA = [0, 1, 2].flatMap((row) => [0, 1, 2].map((column) =>
    matrix[row] * matrix[column] + matrix[3 + row] * matrix[3 + column] + matrix[6 + row] * matrix[6 + column]));
  const eigen = symmetricEigen3([atA[0], atA[1], atA[2], atA[4], atA[5], atA[8]]);
  const order = [2, 1, 0];
  const singularValues = order.map((index) => Math.sqrt(Math.max(0, eigen.values[index]))) as [number, number, number];
  const vColumns = order.map((index) => [...eigen.vectors[index]] as [number, number, number]);
  const v = [vColumns[0][0], vColumns[1][0], vColumns[2][0], vColumns[0][1], vColumns[1][1], vColumns[2][1], vColumns[0][2], vColumns[1][2], vColumns[2][2]];
  const uColumns: [number, number, number][] = [];
  const tolerance = Math.max(1e-14, singularValues[0] * 1e-12);
  for (let column = 0; column < 3; column += 1) {
    if (singularValues[column] > tolerance) {
      const vc = vColumns[column];
      const candidate: [number, number, number] = [
        (matrix[0] * vc[0] + matrix[1] * vc[1] + matrix[2] * vc[2]) / singularValues[column],
        (matrix[3] * vc[0] + matrix[4] * vc[1] + matrix[5] * vc[2]) / singularValues[column],
        (matrix[6] * vc[0] + matrix[7] * vc[1] + matrix[8] * vc[2]) / singularValues[column],
      ];
      for (const previous of uColumns) {
        const dot = candidate[0] * previous[0] + candidate[1] * previous[1] + candidate[2] * previous[2];
        candidate[0] -= dot * previous[0]; candidate[1] -= dot * previous[1]; candidate[2] -= dot * previous[2];
      }
      const length = Math.hypot(...candidate);
      if (length > 1e-10) uColumns.push(candidate.map((value) => value / length) as [number, number, number]);
      else uColumns.push([0, 0, 0]);
    } else {
      uColumns.push([0, 0, 0]);
    }
  }
  if (uColumns.length !== 3) throw new Error('svd3 factor construction failed.');
  if (Math.hypot(...uColumns[0]) < 0.5) uColumns[0] = [1, 0, 0];
  if (Math.hypot(...uColumns[1]) < 0.5) {
    const axis: [number, number, number] = Math.abs(uColumns[0][0]) < 0.8 ? [1, 0, 0] : [0, 1, 0];
    const dot = axis[0] * uColumns[0][0] + axis[1] * uColumns[0][1] + axis[2] * uColumns[0][2];
    const perpendicular: [number, number, number] = [axis[0] - dot * uColumns[0][0], axis[1] - dot * uColumns[0][1], axis[2] - dot * uColumns[0][2]];
    const length = Math.hypot(...perpendicular);
    uColumns[1] = perpendicular.map((value) => value / length) as [number, number, number];
  }
  if (Math.hypot(...uColumns[2]) < 0.5) {
    const a = uColumns[0]; const b = uColumns[1];
    uColumns[2] = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  }
  const U = [uColumns[0][0], uColumns[1][0], uColumns[2][0], uColumns[0][1], uColumns[1][1], uColumns[2][1], uColumns[0][2], uColumns[1][2], uColumns[2][2]] as unknown as Matrix3;
  return { U, singularValues, V: v as unknown as Matrix3 };
}

export interface FusedRegistrationTag {
  tagId: number;
  positionScan: Vector3;
  spreadM: number;
  observationCount: number;
  inlierCount: number;
  confirmed: boolean;
}

export interface RegistrationPodGeometry {
  pod: number;
  computed: boolean;
  assigned?: boolean;
  x_mm?: number;
  y_mm?: number;
  z_mm?: number;
  uncertainty_mm?: number;
}

export interface RegistrationPerNode {
  node: RegistrationNode;
  tagId: number;
  residualM: number | null;
  status: RegistrationNodeStatus;
}

export interface SensorToScanRegistration {
  valid: boolean;
  reason: string | null;
  tier: RegistrationTier | null;
  sensorToScan: number[] | null;
  mirrored: boolean;
  rmsResidualM: number;
  thresholdM: number;
  perNode: RegistrationPerNode[];
}

type Correspondence = { tagId: number; node: RegistrationNode; sensor: Vector3; scan: Vector3; uncertaintyMm?: number };
const NODE_BY_TAG: readonly RegistrationNode[] = ['MAIN', 'G1', 'G2', 'G3', 'G4'];

function multiply3(a: readonly number[], b: readonly number[]): number[] {
  return [0, 1, 2].flatMap((row) => [0, 1, 2].map((column) =>
    a[row * 3] * b[column] + a[row * 3 + 1] * b[3 + column] + a[row * 3 + 2] * b[6 + column]));
}

function transpose3(a: readonly number[]): number[] {
  return [a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]];
}

function determinant3(a: readonly number[]): number {
  return a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
}

function centeredCovariance(points: readonly Vector3[]): number[] {
  const center: [number, number, number] = [0, 1, 2].map((axis) => points.reduce((sum, point) => sum + point[axis], 0) / points.length) as [number, number, number];
  const covariance = Array(9).fill(0) as number[];
  for (const point of points) {
    const d = [point[0] - center[0], point[1] - center[1], point[2] - center[2]];
    for (let row = 0; row < 3; row += 1) for (let column = 0; column < 3; column += 1) covariance[row * 3 + column] += d[row] * d[column] / points.length;
  }
  return covariance;
}

function fitCandidate(points: readonly Correspondence[], parity: 1 | -1): { rotation: number[]; translation: Vector3; rms: number } {
  const sourceCenter: [number, number, number] = [0, 1, 2].map((axis) => points.reduce((sum, point) => sum + point.sensor[axis], 0) / points.length) as [number, number, number];
  const targetCenter: [number, number, number] = [0, 1, 2].map((axis) => points.reduce((sum, point) => sum + point.scan[axis], 0) / points.length) as [number, number, number];
  const cross = Array(9).fill(0) as number[];
  for (const point of points) {
    const a = point.sensor.map((value, axis) => value - sourceCenter[axis]);
    const b = point.scan.map((value, axis) => value - targetCenter[axis]);
    for (let row = 0; row < 3; row += 1) for (let column = 0; column < 3; column += 1) cross[row * 3 + column] += a[row] * b[column];
  }
  const { U, V } = svd3(cross);
  const base = multiply3(V, transpose3(U));
  const correction = [1, 0, 0, 0, 1, 0, 0, 0, parity * Math.sign(determinant3(base))];
  const rotation = multiply3(multiply3(V, correction), transpose3(U));
  const translation: [number, number, number] = [0, 1, 2].map((axis) =>
    targetCenter[axis] - rotation[axis * 3] * sourceCenter[0] - rotation[axis * 3 + 1] * sourceCenter[1] - rotation[axis * 3 + 2] * sourceCenter[2]) as [number, number, number];
  const squared = points.reduce((sum, point) => {
    const dx = rotation[0] * point.sensor[0] + rotation[1] * point.sensor[1] + rotation[2] * point.sensor[2] + translation[0] - point.scan[0];
    const dy = rotation[3] * point.sensor[0] + rotation[4] * point.sensor[1] + rotation[5] * point.sensor[2] + translation[1] - point.scan[1];
    const dz = rotation[6] * point.sensor[0] + rotation[7] * point.sensor[1] + rotation[8] * point.sensor[2] + translation[2] - point.scan[2];
    return sum + dx * dx + dy * dy + dz * dz;
  }, 0);
  return { rotation, translation, rms: Math.sqrt(squared / points.length) };
}

export type FusedTagsById = Partial<Record<0 | 1 | 2 | 3 | 4, FusedRegistrationTag>>;

function tagMap(tags: readonly FusedRegistrationTag[] | FusedTagsById): Map<number, FusedRegistrationTag> {
  const map = new Map<number, FusedRegistrationTag>();
  const values: readonly (FusedRegistrationTag | undefined)[] = Array.isArray(tags) ? tags : Object.values(tags);
  for (const tag of values) if (tag !== undefined && Number.isInteger(tag.tagId) && tag.tagId >= 0 && tag.tagId <= 4 && tag.confirmed && !map.has(tag.tagId)) map.set(tag.tagId, tag);
  return map;
}

/**
 * Fits scan = R * sensor + t. Sensor positions are converted from millimetres
 * to metres. The chosen proper/reflected fit must map sensor +Z toward scan +Y
 * within maxTiltDeg (default 30 degrees because the nodes are leveled and XR
 * gravity aligns the scan frame); collinearity uses 1e-9 of the maximum
 * centered sensor-position covariance eigenvalue.
 */
export function registerSensorToScan(input: {
  tags: readonly FusedRegistrationTag[] | FusedTagsById;
  geometry: { pods: readonly RegistrationPodGeometry[]; available?: boolean };
  geometryComputedAtMs: number;
  thresholdFloorM?: number;
  /** Maximum angle between sensor +Z and scan +Y; defaults to 30 degrees. */
  maxTiltDeg?: number;
}): SensorToScanRegistration {
  const tags = tagMap(input.tags);
  const pods = new Map(input.geometry.pods.map((pod) => [pod.pod, pod]));
  const computedPods = [...pods.values()].filter((pod) => pod.computed && [pod.x_mm, pod.y_mm, pod.z_mm].every((value) => Number.isFinite(value)));
  const correspondences: Correspondence[] = [];
  const mainTag = tags.get(0);
  if (mainTag !== undefined) correspondences.push({ tagId: 0, node: 'MAIN', sensor: [0, 0, 0], scan: mainTag.positionScan });
  for (let tagId = 1; tagId <= 4; tagId += 1) {
    const tag = tags.get(tagId); const pod = pods.get(tagId - 1);
    if (tag !== undefined && pod?.computed === true && [pod.x_mm, pod.y_mm, pod.z_mm].every((value) => Number.isFinite(value))) {
      correspondences.push({
        tagId, node: NODE_BY_TAG[tagId],
        sensor: [pod.x_mm! / 1000, pod.y_mm! / 1000, pod.z_mm! / 1000],
        scan: tag.positionScan,
        ...(Number.isFinite(pod.uncertainty_mm) && (pod.uncertainty_mm ?? 0) > 0 && (pod.uncertainty_mm ?? 0) < 2147483647 ? { uncertaintyMm: pod.uncertainty_mm } : {}),
      });
    }
  }
  const uncertainties = correspondences.flatMap((point) => point.uncertaintyMm === undefined ? [] : [point.uncertaintyMm]);
  const uncertaintyRmsM = uncertainties.length === 0 ? 0 : Math.sqrt(uncertainties.reduce((sum, value) => sum + value * value, 0) / uncertainties.length) / 1000;
  const thresholdM = Math.max(input.thresholdFloorM ?? 0.05, 3 * uncertaintyRmsM);
  const perNode: RegistrationPerNode[] = [...tags.keys()].sort((a, b) => a - b).map((tagId) => ({
    node: NODE_BY_TAG[tagId], tagId, residualM: null, status: 'visual_only',
  }));
  const noTransform = (reason: string): SensorToScanRegistration => ({ valid: false, reason, tier: null, sensorToScan: null, mirrored: false, rmsResidualM: 0, thresholdM, perNode });

  if (input.geometry.available === false || computedPods.length === 0) return noTransform('no_geometry');
  if (mainTag === undefined) return noTransform('missing_main');
  const remoteCount = correspondences.filter((point) => point.tagId > 0).length;
  if (remoteCount < 3) return noTransform('too_few_remotes');

  const spread = svd3(centeredCovariance(correspondences.map((point) => point.sensor))).singularValues;
  if (spread[0] <= 1e-12 || spread[1] <= Math.max(1e-12, spread[0] * 1e-9)) return noTransform('collinear');
  const proper = fitCandidate(correspondences, 1);
  const reflected = fitCandidate(correspondences, -1);
  const minimumGravityDot = Math.cos((input.maxTiltDeg ?? 30) * Math.PI / 180);
  const admissible = [proper, reflected].filter((candidate) => candidate.rotation[5] >= minimumGravityDot);
  if (admissible.length === 0) return noTransform('gravity_inconsistent');
  const best = admissible.reduce((lowerRms, candidate) => candidate.rms < lowerRms.rms ? candidate : lowerRms);
  const determinant = determinant3(best.rotation);
  const matrix = [
    ...best.rotation.slice(0, 3), best.translation[0],
    ...best.rotation.slice(3, 6), best.translation[1],
    ...best.rotation.slice(6, 9), best.translation[2],
    0, 0, 0, 1,
  ];
  const valid = best.rms <= thresholdM;
  const expectedPods = [...pods.values()].filter((pod) => pod.assigned === true || (pod.assigned === undefined && pod.computed));
  const full = tags.has(0) && expectedPods.every((pod) => tags.has(pod.pod + 1));
  const fittedByTag = new Map<number, number>();
  correspondences.forEach((point) => {
    const dx = best.rotation[0] * point.sensor[0] + best.rotation[1] * point.sensor[1] + best.rotation[2] * point.sensor[2] + best.translation[0] - point.scan[0];
    const dy = best.rotation[3] * point.sensor[0] + best.rotation[4] * point.sensor[1] + best.rotation[5] * point.sensor[2] + best.translation[1] - point.scan[1];
    const dz = best.rotation[6] * point.sensor[0] + best.rotation[7] * point.sensor[1] + best.rotation[8] * point.sensor[2] + best.translation[2] - point.scan[2];
    fittedByTag.set(point.tagId, Math.hypot(dx, dy, dz));
  });
  const resultPerNode: RegistrationPerNode[] = perNode.map((entry) => {
    const status: RegistrationNodeStatus = fittedByTag.has(entry.tagId) ? (valid ? 'anchored' : 'provisional') : 'visual_only';
    return { ...entry, residualM: fittedByTag.get(entry.tagId) ?? null, status };
  });
  return {
    valid,
    reason: valid ? null : 'residual_too_high',
    tier: valid ? (full ? 'full' : 'minimum') : null,
    sensorToScan: matrix,
    mirrored: determinant < 0,
    rmsResidualM: best.rms,
    thresholdM,
    perNode: resultPerNode,
  };
}

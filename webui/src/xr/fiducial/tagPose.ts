export type NormalizedTagCorner = readonly [u: number, v: number];
export type Matrix3 = readonly [number, number, number, number, number, number, number, number, number];

export interface TagPoseInput {
  /**
   * AprilTag's ordered corners: counter-clockwise, starting at bottom-left
   * when viewed in the tag's own frame (the usual tag36h11 convention).
   * Image v is top-down, so a correctly ordered visible tag has negative area.
   */
  corners: readonly [NormalizedTagCorner, NormalizedTagCorner, NormalizedTagCorner, NormalizedTagCorner];
  /** Column-major WebXR projection matrix. */
  projectionMatrix: ArrayLike<number>;
  /** Column-major transform from WebXR view space to the scan frame. */
  worldFromView: ArrayLike<number>;
  /** Edge length of the black square in metres. */
  tagSizeM: number;
  /** Minimum inferred corner depth in front of the camera, in metres; defaults to 0.05. */
  minDepthM?: number;
}

export type TagPoseResult =
  | { ok: true; positionScan: [number, number, number]; rotationScan: Matrix3; /** RMS 2D corner error in normalized image coordinates. */ reprojectionErrorNorm: number; distanceM: number }
  | { ok: false; reason: string };

type Vec2 = [number, number];
type Vec3 = [number, number, number];
type Pose = { rotation: number[]; translation: Vec3; error: number };

function solveLinear(matrix: number[][], rhs: number[]): number[] | null {
  const n = rhs.length;
  const a = matrix.map((row, index) => [...row, rhs[index]]);
  const scale = Math.max(0, ...a.flat().map(Math.abs));
  if (!(scale > 0)) return null;
  for (let column = 0; column < n; column += 1) {
    let pivot = column;
    for (let row = column + 1; row < n; row += 1) {
      if (Math.abs(a[row][column]) > Math.abs(a[pivot][column])) pivot = row;
    }
    if (Math.abs(a[pivot][column]) <= scale * 1e-13) return null;
    [a[column], a[pivot]] = [a[pivot], a[column]];
    const divisor = a[column][column];
    for (let j = column; j <= n; j += 1) a[column][j] /= divisor;
    for (let row = 0; row < n; row += 1) {
      if (row === column) continue;
      const factor = a[row][column];
      for (let j = column; j <= n; j += 1) a[row][j] -= factor * a[column][j];
    }
  }
  return a.map((row) => row[n]);
}

function inverse4(matrix: ArrayLike<number>): number[] | null {
  const rows = Array.from({ length: 4 }, (_, row) => [
    ...Array.from({ length: 4 }, (_, column) => Number(matrix[column * 4 + row])),
    ...Array.from({ length: 4 }, (_, column) => Number(row === column)),
  ]);
  const scale = Math.max(...rows.flat().slice(0, 4).map(Math.abs));
  if (!(scale > 0)) return null;
  for (let column = 0; column < 4; column += 1) {
    let pivot = column;
    for (let row = column + 1; row < 4; row += 1) {
      if (Math.abs(rows[row][column]) > Math.abs(rows[pivot][column])) pivot = row;
    }
    if (Math.abs(rows[pivot][column]) <= scale * 1e-14) return null;
    [rows[column], rows[pivot]] = [rows[pivot], rows[column]];
    const divisor = rows[column][column];
    for (let j = 0; j < 8; j += 1) rows[column][j] /= divisor;
    for (let row = 0; row < 4; row += 1) {
      if (row === column) continue;
      const factor = rows[row][column];
      for (let j = 0; j < 8; j += 1) rows[row][j] -= factor * rows[column][j];
    }
  }
  return Array.from({ length: 16 }, (_, index) => rows[index % 4][4 + Math.floor(index / 4)]);
}

function orientation(a: Vec2, b: Vec2, c: Vec2): number {
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

function segmentsIntersect(a: Vec2, b: Vec2, c: Vec2, d: Vec2): boolean {
  const o1 = orientation(a, b, c); const o2 = orientation(a, b, d);
  const o3 = orientation(c, d, a); const o4 = orientation(c, d, b);
  return o1 * o2 < 0 && o3 * o4 < 0;
}

function validQuad(corners: readonly Vec2[]): string | null {
  if (segmentsIntersect(corners[0], corners[1], corners[2], corners[3]) ||
      segmentsIntersect(corners[1], corners[2], corners[3], corners[0])) return 'self_intersecting';
  let area2 = 0;
  for (let i = 0; i < 4; i += 1) {
    const a = corners[i]; const b = corners[(i + 1) % 4];
    area2 += a[0] * b[1] - a[1] * b[0];
  }
  if (Math.abs(area2) < 1e-12) return 'degenerate_corners';
  // Four projected corners cannot distinguish a rear projection from a front-facing tag.
  // A physically flipped tag reverses the ordered corners and is rejected here.
  if (area2 > 0) return 'wrong_winding';
  let sign = 0;
  for (let i = 0; i < 4; i += 1) {
    const turn = orientation(corners[i], corners[(i + 1) % 4], corners[(i + 2) % 4]);
    if (Math.abs(turn) < 1e-12) return 'degenerate_corners';
    const current = Math.sign(turn);
    if (sign !== 0 && current !== sign) return 'degenerate_corners';
    sign = current;
  }
  return null;
}

function unprojectRays(corners: readonly Vec2[], projectionInverse: readonly number[]): Vec2[] | null {
  const result: Vec2[] = [];
  for (const [u, v] of corners) {
    const clip: Vec4 = [2 * u - 1, 1 - 2 * v, -1, 1];
    const point = [0, 1, 2, 3].map((row) =>
      projectionInverse[row] * clip[0] + projectionInverse[4 + row] * clip[1] +
      projectionInverse[8 + row] * clip[2] + projectionInverse[12 + row] * clip[3]);
    const w = point[3];
    if (!Number.isFinite(w) || Math.abs(w) < 1e-14) return null;
    // OpenCV-style camera coordinates are (view x, -view y, -view z).
    const x = point[0] / w; const y = -point[1] / w; const z = -point[2] / w;
    if (![x, y, z].every(Number.isFinite) || z <= 1e-12) return null;
    result.push([x / z, y / z]);
  }
  return result;
}

type Vec4 = [number, number, number, number];

function squareHomography(points: readonly Vec2[], halfSize: number): number[] | null {
  const object: Vec2[] = [[-halfSize, -halfSize], [halfSize, -halfSize], [halfSize, halfSize], [-halfSize, halfSize]];
  const a: number[][] = []; const b: number[] = [];
  for (let index = 0; index < 4; index += 1) {
    const [x, y] = object[index]; const [u, v] = points[index];
    a.push([x, y, 1, 0, 0, 0, -u * x, -u * y]); b.push(u);
    a.push([0, 0, 0, x, y, 1, -v * x, -v * y]); b.push(v);
  }
  const values = solveLinear(a, b);
  return values === null ? null : [...values, 1];
}

function rotateZTo(direction: Vec3): number[] {
  const length = Math.hypot(...direction);
  const [x, y, z] = direction.map((value) => value / length) as Vec3;
  if (1 + z < 1e-12) return [1, 0, 0, 0, 1, 0, 0, 0, -1];
  const d = 1 / (1 + z);
  return [1 - x * x * d, -x * y * d, -x, -x * y * d, 1 - y * y * d, -y, x, y, 1 - (x * x + y * y) * d];
}

function ippeRotations(h: readonly number[]): number[][] | null {
  const p = h[2]; const q = h[5];
  const j00 = h[0] - h[6] * p; const j01 = h[1] - h[7] * p;
  const j10 = h[3] - h[6] * q; const j11 = h[4] - h[7] * q;
  const alignTranspose = rotateZTo([p, q, 1]);
  const rv = [alignTranspose[0], alignTranspose[3], alignTranspose[6],
    alignTranspose[1], alignTranspose[4], alignTranspose[7],
    alignTranspose[2], alignTranspose[5], alignTranspose[8]];
  const b00 = rv[0] - p * rv[6]; const b01 = rv[1] - p * rv[7];
  const b10 = rv[3] - q * rv[6]; const b11 = rv[4] - q * rv[7];
  const det = b00 * b11 - b01 * b10;
  if (Math.abs(det) < 1e-14) return null;
  const a00 = (b11 * j00 - b01 * j10) / det;
  const a01 = (b11 * j01 - b01 * j11) / det;
  const a10 = (-b10 * j00 + b00 * j10) / det;
  const a11 = (-b10 * j01 + b00 * j11) / det;
  const ata00 = a00 * a00 + a01 * a01; const ata01 = a00 * a10 + a01 * a11;
  const ata11 = a10 * a10 + a11 * a11;
  const gamma = Math.sqrt(0.5 * (ata00 + ata11 + Math.hypot(ata00 - ata11, 2 * ata01)));
  if (!(gamma > 1e-14) || !Number.isFinite(gamma)) return null;
  const m00 = a00 / gamma; const m01 = a01 / gamma;
  const m10 = a10 / gamma; const m11 = a11 / gamma;
  const c0 = Math.sqrt(Math.max(0, 1 - m00 * m00 - m10 * m10));
  let c1 = Math.sqrt(Math.max(0, 1 - m01 * m01 - m11 * m11));
  if (-(m00 * m01 + m10 * m11) < 0) c1 = -c1;
  const cross = (x: Vec3, y: Vec3): Vec3 => [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]];
  const make = (s: number): number[] => {
    const first: Vec3 = [m00, m10, s * c0]; const second: Vec3 = [m01, m11, s * c1];
    const third = cross(first, second);
    const right = [first[0], second[0], third[0], first[1], second[1], third[1], first[2], second[2], third[2]];
    return [0, 1, 2].flatMap((row) => [0, 1, 2].map((column) =>
      rv[row * 3] * right[column] + rv[row * 3 + 1] * right[3 + column] + rv[row * 3 + 2] * right[6 + column]));
  };
  return [make(1), make(-1)];
}

function translationFor(rotation: readonly number[], points: readonly Vec2[], size: number): Vec3 | null {
  const half = size / 2;
  const object: Vec2[] = [[-half, -half], [half, -half], [half, half], [-half, half]];
  const rows: number[][] = []; const rhs: number[] = [];
  for (let index = 0; index < 4; index += 1) {
    const [x, y] = object[index]; const [u, v] = points[index];
    const rx = rotation[0] * x + rotation[1] * y;
    const ry = rotation[3] * x + rotation[4] * y;
    const rz = rotation[6] * x + rotation[7] * y;
    rows.push([1, 0, -u]); rhs.push(u * rz - rx);
    rows.push([0, 1, -v]); rhs.push(v * rz - ry);
  }
  const normal = Array.from({ length: 3 }, (_, i) => Array.from({ length: 3 }, (_, j) =>
    rows.reduce((sum, row) => sum + row[i] * row[j], 0)));
  const vector = Array.from({ length: 3 }, (_, i) => rows.reduce((sum, row, index) => sum + row[i] * rhs[index], 0));
  return solveLinear(normal, vector) as Vec3 | null;
}

function candidateError(rotation: readonly number[], translation: Vec3, size: number, projection: ArrayLike<number>, corners: readonly Vec2[]): number {
  const half = size / 2;
  const object: Vec2[] = [[-half, -half], [half, -half], [half, half], [-half, half]];
  let squared = 0;
  for (let index = 0; index < 4; index += 1) {
    const [x, y] = object[index];
    const vx = rotation[0] * x + rotation[1] * y + translation[0];
    const vy = rotation[3] * x + rotation[4] * y + translation[1];
    const vz = rotation[6] * x + rotation[7] * y + translation[2];
    if (Math.abs(vz) < 1e-10) return Number.POSITIVE_INFINITY;
    const cx = Number(projection[0]) * vx + Number(projection[4]) * vy + Number(projection[8]) * vz + Number(projection[12]);
    const cy = Number(projection[1]) * vx + Number(projection[5]) * vy + Number(projection[9]) * vz + Number(projection[13]);
    const cw = Number(projection[3]) * vx + Number(projection[7]) * vy + Number(projection[11]) * vz + Number(projection[15]);
    if (Math.abs(cw) < 1e-14) return Number.POSITIVE_INFINITY;
    const du = (cx / cw + 1) / 2 - corners[index][0];
    const dv = (1 - cy / cw) / 2 - corners[index][1];
    squared += du * du + dv * dv;
  }
  return Math.sqrt(squared / 4);
}

export function estimateTagPose(input: TagPoseInput): TagPoseResult {
  const { corners, projectionMatrix: projection, worldFromView, tagSizeM } = input;
  const minDepthM = input.minDepthM ?? 0.05;
  if (!Number.isFinite(tagSizeM) || !Array.isArray(corners) || corners.length !== 4 ||
      !Number.isFinite(minDepthM) ||
      !projection || projection.length !== 16 || !worldFromView || worldFromView.length !== 16 ||
      Array.from({ length: 16 }, (_, i) => Number(projection[i])).some((v) => !Number.isFinite(v)) ||
      Array.from({ length: 16 }, (_, i) => Number(worldFromView[i])).some((v) => !Number.isFinite(v)) ||
      corners.some((corner) => !Array.isArray(corner) || corner.length !== 2 || corner.some((v) => !Number.isFinite(v)))) {
    return { ok: false, reason: 'non_finite_input' };
  }
  if (tagSizeM <= 0) return { ok: false, reason: 'invalid_tag_size' };
  if (minDepthM < 0) return { ok: false, reason: 'invalid_min_depth' };
  const quadReason = validQuad(corners);
  if (quadReason !== null) return { ok: false, reason: quadReason };
  const inverseProjection = inverse4(projection);
  if (inverseProjection === null) return { ok: false, reason: 'singular_projection' };
  const imageRays = unprojectRays(corners, inverseProjection);
  if (imageRays === null) return { ok: false, reason: 'singular_projection' };
  const homography = squareHomography(imageRays, tagSizeM / 2);
  if (homography === null) return { ok: false, reason: 'degenerate_corners' };
  const rotations = ippeRotations(homography);
  if (rotations === null) return { ok: false, reason: 'degenerate_corners' };
  const candidates: Pose[] = [];
  for (const cameraRotation of rotations) {
    const translationCamera = translationFor(cameraRotation, imageRays, tagSizeM);
    if (translationCamera === null) continue;
    // CV (x-right, y-down, z-forward) to WebXR (x-right, y-up, z-back).
    const rotationView = cameraRotation.map((value, index) => value * (Math.floor(index / 3) === 0 ? 1 : -1));
    const translationView: Vec3 = [translationCamera[0], -translationCamera[1], -translationCamera[2]];
    const error = candidateError(rotationView, translationView, tagSizeM, projection, corners);
    if (!Number.isFinite(error)) continue;
    candidates.push({ rotation: rotationView, translation: translationView, error });
  }
  if (candidates.length === 0) return { ok: false, reason: 'too_close_or_behind' };
  candidates.sort((a, b) => a.error - b.error);
  const best = candidates[0];
  const half = tagSizeM / 2;
  const objectCorners: Vec2[] = [[-half, -half], [half, -half], [half, half], [-half, half]];
  // WebXR looks along -Z. This guards against inferred corners too close to or across the camera plane.
  if (objectCorners.some(([x, y]) => !(best.rotation[6] * x + best.rotation[7] * y + best.translation[2] <= -minDepthM))) {
    return { ok: false, reason: 'too_close_or_behind' };
  }
  const worldRotation = Array.from({ length: 9 }, (_, index) => {
    const row = Math.floor(index / 3); const column = index % 3;
    return Number(worldFromView[row]) * best.rotation[column] +
      Number(worldFromView[4 + row]) * best.rotation[3 + column] +
      Number(worldFromView[8 + row]) * best.rotation[6 + column];
  }) as unknown as Matrix3;
  const [x, y, z] = best.translation;
  const positionScan: [number, number, number] = [
    Number(worldFromView[0]) * x + Number(worldFromView[4]) * y + Number(worldFromView[8]) * z + Number(worldFromView[12]),
    Number(worldFromView[1]) * x + Number(worldFromView[5]) * y + Number(worldFromView[9]) * z + Number(worldFromView[13]),
    Number(worldFromView[2]) * x + Number(worldFromView[6]) * y + Number(worldFromView[10]) * z + Number(worldFromView[14]),
  ];
  return { ok: true, positionScan, rotationScan: worldRotation, reprojectionErrorNorm: best.error, distanceM: Math.hypot(x, y, z) };
}

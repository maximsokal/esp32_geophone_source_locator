export type ProjectVector = readonly [number, number, number];

export interface IndependentViewUpdate {
  independent: boolean;
  packed: number;
  count: number;
  code: number;
  minimumAngleRadians: number;
}

export interface DepthNeighbourhoodQuality {
  accepted: boolean;
  score: number;
  edge: boolean;
  dominantAxis: 'u' | 'v' | 'none';
  supportingNeighbours: number;
}

export interface SurfaceConfidenceInput {
  promoted: boolean;
  independentViews: number;
  weight: number;
  residualMeters: number;
  maxViewAngleRadians: number;
  contradictionWeight?: number;
}

const VIEW_AZIMUTH_BINS = 64;
const VIEW_ELEVATION_BINS = 32;
const VIEW_CODE_BASE = VIEW_AZIMUTH_BINS * VIEW_ELEVATION_BINS; // 2048 = 11 bits
const MAX_PACKED_VIEW_CODES = 4; // 44 bits, exactly representable by JS Number
export const DEFAULT_INDEPENDENT_VIEW_ANGLE_RADIANS = (8 * Math.PI) / 180;

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

export function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

export function normalizeVector(vector: ProjectVector): ProjectVector {
  const length = Math.hypot(vector[0], vector[1], vector[2]);
  if (!Number.isFinite(length) || length < 1e-9) return [0, 0, 1];
  return [vector[0] / length, vector[1] / length, vector[2] / length];
}

export function angularDistance(a: ProjectVector, b: ProjectVector): number {
  const na = normalizeVector(a);
  const nb = normalizeVector(b);
  return Math.acos(clamp(na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2], -1, 1));
}

export function encodeViewDirection(direction: ProjectVector): number {
  const [x, y, z] = normalizeVector(direction);
  const azimuth01 = (Math.atan2(y, x) + Math.PI) / (Math.PI * 2);
  const elevation01 = (Math.asin(clamp(z, -1, 1)) + Math.PI / 2) / Math.PI;
  const azimuthBin = clamp(Math.floor(azimuth01 * VIEW_AZIMUTH_BINS), 0, VIEW_AZIMUTH_BINS - 1);
  const elevationBin = clamp(Math.floor(elevation01 * VIEW_ELEVATION_BINS), 0, VIEW_ELEVATION_BINS - 1);
  return elevationBin * VIEW_AZIMUTH_BINS + azimuthBin;
}

export function decodeViewDirection(code: number): ProjectVector {
  const safeCode = clamp(Math.floor(code), 0, VIEW_CODE_BASE - 1);
  const elevationBin = Math.floor(safeCode / VIEW_AZIMUTH_BINS);
  const azimuthBin = safeCode % VIEW_AZIMUTH_BINS;
  const azimuth = ((azimuthBin + 0.5) / VIEW_AZIMUTH_BINS) * Math.PI * 2 - Math.PI;
  const elevation = ((elevationBin + 0.5) / VIEW_ELEVATION_BINS) * Math.PI - Math.PI / 2;
  const cosElevation = Math.cos(elevation);
  return [cosElevation * Math.cos(azimuth), cosElevation * Math.sin(azimuth), Math.sin(elevation)];
}

export function packInitialView(direction: ProjectVector): { packed: number; count: number } {
  return { packed: encodeViewDirection(direction), count: 1 };
}

function packedViewCode(packed: number, index: number): number {
  return Math.floor(packed / (VIEW_CODE_BASE ** index)) % VIEW_CODE_BASE;
}

/**
 * Keeps up to four quantized view directions in one 44-bit JS number. This is
 * much more reliable than treating a new global keyframe id as a new view,
 * while avoiding 9-12 extra floating-point fields per retained surfel.
 */
export function registerIndependentView(
  packed: number,
  count: number,
  candidateDirection: ProjectVector,
  minAngleRadians = DEFAULT_INDEPENDENT_VIEW_ANGLE_RADIANS,
): IndependentViewUpdate {
  const safeCount = clamp(Math.floor(count), 0, MAX_PACKED_VIEW_CODES);
  const code = encodeViewDirection(candidateDirection);
  const candidate = normalizeVector(candidateDirection);
  let minimumAngleRadians = Number.POSITIVE_INFINITY;
  for (let index = 0; index < safeCount; index += 1) {
    const existing = decodeViewDirection(packedViewCode(packed, index));
    minimumAngleRadians = Math.min(minimumAngleRadians, angularDistance(existing, candidate));
  }
  if (safeCount === 0) minimumAngleRadians = Math.PI;
  const independent = safeCount < MAX_PACKED_VIEW_CODES && minimumAngleRadians >= minAngleRadians;
  if (!independent) return { independent: false, packed, count: safeCount, code, minimumAngleRadians };
  const updated = packed + code * (VIEW_CODE_BASE ** safeCount);
  return { independent: true, packed: updated, count: safeCount + 1, code, minimumAngleRadians };
}

function median(values: number[]): number {
  values.sort((a, b) => a - b);
  const middle = Math.floor(values.length / 2);
  return values.length % 2 === 0
    ? (values[middle - 1] + values[middle]) * 0.5
    : values[middle];
}

export function depthToleranceMeters(depthMeters: number): number {
  return clamp(0.020 + 0.0040 * depthMeters * depthMeters, 0.026, 0.105);
}

/**
 * Edge-aware depth validation.
 *
 * A true silhouette/room corner has neighbours on two depth layers. Rejecting
 * the center merely because the neighbourhood median is far away erases the
 * geometry we care about most. We therefore require same-layer support instead:
 * two supporting neighbours = normal surface; one supporting neighbour plus a
 * clear discontinuity = accepted edge; no supporting neighbours = isolated
 * spike/outlier.
 */
export function classifyDepthNeighbourhood(
  centerDepth: number,
  left: number | null,
  right: number | null,
  up: number | null,
  down: number | null,
): DepthNeighbourhoodQuality {
  if (!Number.isFinite(centerDepth) || centerDepth <= 0) {
    return { accepted: false, score: 0, edge: false, dominantAxis: 'none', supportingNeighbours: 0 };
  }

  const tolerance = depthToleranceMeters(centerDepth);
  const entries: Array<{ axis: 'u' | 'v'; value: number }> = [];
  const add = (axis: 'u' | 'v', value: number | null): void => {
    if (value !== null && Number.isFinite(value) && value > 0) entries.push({ axis, value });
  };
  add('u', left); add('u', right); add('v', up); add('v', down);

  if (entries.length < 2) {
    return { accepted: true, score: 0.38, edge: false, dominantAxis: 'none', supportingNeighbours: entries.length };
  }

  const residuals = entries.map((entry) => Math.abs(entry.value - centerDepth));
  const support = residuals.filter((residual) => residual <= tolerance);
  const discontinuityGate = Math.max(0.075, tolerance * 2.2);
  const far = entries.filter((entry) => Math.abs(entry.value - centerDepth) >= discontinuityGate);
  const edge = far.length > 0;

  let dominantAxis: 'u' | 'v' | 'none' = 'none';
  if (edge) {
    const uJump = Math.max(
      left === null ? 0 : Math.abs(left - centerDepth),
      right === null ? 0 : Math.abs(right - centerDepth),
    );
    const vJump = Math.max(
      up === null ? 0 : Math.abs(up - centerDepth),
      down === null ? 0 : Math.abs(down - centerDepth),
    );
    dominantAxis = uJump >= vJump ? 'u' : 'v';
  }

  if (support.length >= 2) {
    const inlierMedian = median(support.slice());
    const residualScore = 1 - clamp01(inlierMedian / tolerance);
    return {
      accepted: true,
      score: clamp01((edge ? 0.62 : 0.78) + residualScore * (edge ? 0.30 : 0.22)),
      edge,
      dominantAxis,
      supportingNeighbours: support.length,
    };
  }

  if (support.length === 1 && edge) {
    return {
      accepted: true,
      score: 0.52,
      edge: true,
      dominantAxis,
      supportingNeighbours: 1,
    };
  }

  return {
    accepted: false,
    score: 0,
    edge,
    dominantAxis,
    supportingNeighbours: support.length,
  };
}

function hash32(value: number): number {
  let x = value | 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d);
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b);
  x ^= x >>> 16;
  return x >>> 0;
}

/** Returns a deterministic coordinate inside one stratum, not at its fixed center. */
export function stratifiedSampleCoordinate(
  index: number,
  count: number,
  margin: number,
  phase: number,
  axisSalt: number,
): number {
  const safeCount = Math.max(1, Math.floor(count));
  const safeIndex = clamp(Math.floor(index), 0, safeCount - 1);
  const usableSpan = 1 - margin * 2;
  const hash = hash32((phase + 1) * 0x45d9f3b + safeIndex * 0x27d4eb2d + axisSalt * 0x165667b1);
  const jitter01 = (hash + 0.5) / 4294967296;
  // Keep away from stratum borders: the same cell is still represented every frame,
  // but repeated keyframes no longer stamp a permanent regular lattice.
  const withinCell = 0.18 + jitter01 * 0.64;
  return margin + ((safeIndex + withinCell) / safeCount) * usableSpan;
}

export function surfaceRenderConfidence(input: SurfaceConfidenceInput): number {
  const viewScore = clamp01((input.independentViews - 1) / 2);
  const angleScore = clamp01(input.maxViewAngleRadians / ((16 * Math.PI) / 180));
  const residualScore = 1 - clamp01(input.residualMeters / 0.055);
  const weightScore = clamp01(input.weight / 4);
  const contradictionPenalty = clamp01((input.contradictionWeight ?? 0) / 3);

  const raw = clamp01(
    0.10 +
    viewScore * 0.34 +
    angleScore * 0.24 +
    residualScore * 0.20 +
    weightScore * 0.12 -
    contradictionPenalty * 0.35,
  );

  return input.promoted ? raw : Math.min(0.48, raw * 0.72);
}

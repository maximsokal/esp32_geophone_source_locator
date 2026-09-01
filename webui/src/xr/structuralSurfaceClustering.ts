import type {
  StructuralGuidePointCloudSnapshot,
  ProjectPoint,
} from './depthPointCloudV2';

type Vector3 = readonly [number, number, number];

interface Family {
  cellKey: string;
  weight: number;
  confidenceWeighted: number;
  positionWeighted: [number, number, number];
  normalWeighted: [number, number, number];
  referenceNormal: Vector3;
  members: number;
}

const MAX_FAMILIES_PER_CELL = 3;
const FAMILY_NORMAL_DOT = Math.cos((38 * Math.PI) / 180);
const MIN_CLUSTER_CONFIDENCE = 0.42;

function clamp01(value: number): number { return Math.max(0, Math.min(1, value)); }
function normalize(x: number, y: number, z: number): Vector3 | null {
  const length = Math.hypot(x, y, z);
  return Number.isFinite(length) && length > 1e-7 ? [x / length, y / length, z / length] : null;
}
function dot(a: Vector3, b: Vector3): number { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function canonical(normal: Vector3): Vector3 {
  const ax = Math.abs(normal[0]), ay = Math.abs(normal[1]), az = Math.abs(normal[2]);
  const axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
  return normal[axis] < 0 ? [-normal[0], -normal[1], -normal[2]] : normal;
}
function cellKey(point: ProjectPoint, size: number): string {
  return `${Math.floor(point[0] / size)}:${Math.floor(point[1] / size)}:${Math.floor(point[2] / size)}`;
}

/**
 * Collapse promoted surfels into up to three orientation families per fine
 * structural cell. This is intentionally downstream of live fusion: the phone
 * can retain its bounded display/memory policy while structural inference sees
 * separate wall/floor/side hypotheses instead of averaging them into a diagonal
 * normal. Opposite normal signs belong to the same physical surface family.
 */
export function clusterStructuralSurfaceFamilies(
  source: StructuralGuidePointCloudSnapshot,
): StructuralGuidePointCloudSnapshot {
  const count = Math.min(
    source.pointCount,
    Math.floor(source.positions.length / 3),
    Math.floor(source.normals.length / 3),
    source.confidence.length,
  );
  if (count === 0) return source;

  const cellSize = Math.max(0.045, Math.min(0.075, source.voxelSizeMeters * 1.35));
  const cells = new Map<string, Family[]>();

  for (let index = 0; index < count; index += 1) {
    const confidence = clamp01(Number(source.confidence[index]));
    if (!Number.isFinite(confidence) || confidence < MIN_CLUSTER_CONFIDENCE) continue;
    const offset = index * 3;
    const position: ProjectPoint = [
      source.positions[offset],
      source.positions[offset + 1],
      source.positions[offset + 2],
    ];
    if (!position.every(Number.isFinite)) continue;
    const rawNormal = normalize(
      source.normals[offset],
      source.normals[offset + 1],
      source.normals[offset + 2],
    );
    if (rawNormal === null) continue;
    const normal = canonical(rawNormal);
    const key = cellKey(position, cellSize);
    const families = cells.get(key) ?? [];

    let best: Family | null = null;
    let bestDot = FAMILY_NORMAL_DOT;
    for (const family of families) {
      const similarity = Math.abs(dot(family.referenceNormal, normal));
      if (similarity >= bestDot) { best = family; bestDot = similarity; }
    }

    if (best === null) {
      if (families.length >= MAX_FAMILIES_PER_CELL) {
        // A fourth conflicting orientation in one tiny cell is more likely
        // mixed/flying evidence than a real fourth manifold. Keep the three
        // strongest established families rather than rotating them frame-to-frame.
        continue;
      }
      const created: Family = {
        cellKey: key,
        weight: confidence,
        confidenceWeighted: confidence * confidence,
        positionWeighted: [position[0] * confidence, position[1] * confidence, position[2] * confidence],
        normalWeighted: [normal[0] * confidence, normal[1] * confidence, normal[2] * confidence],
        referenceNormal: normal,
        members: 1,
      };
      families.push(created);
      cells.set(key, families);
      continue;
    }

    // Align sign to the established family before accumulation.
    const aligned: Vector3 = dot(best.referenceNormal, normal) < 0
      ? [-normal[0], -normal[1], -normal[2]]
      : normal;
    best.weight += confidence;
    best.confidenceWeighted += confidence * confidence;
    best.positionWeighted[0] += position[0] * confidence;
    best.positionWeighted[1] += position[1] * confidence;
    best.positionWeighted[2] += position[2] * confidence;
    best.normalWeighted[0] += aligned[0] * confidence;
    best.normalWeighted[1] += aligned[1] * confidence;
    best.normalWeighted[2] += aligned[2] * confidence;
    best.members += 1;
    const updated = normalize(best.normalWeighted[0], best.normalWeighted[1], best.normalWeighted[2]);
    if (updated !== null) best.referenceNormal = canonical(updated);
  }

  const families = [...cells.values()].flat();
  if (families.length === 0) return source;
  const positions = new Float32Array(families.length * 3);
  const normals = new Float32Array(families.length * 3);
  const confidence = new Float32Array(families.length);
  let write = 0;
  for (const family of families) {
    if (family.weight <= 1e-8) continue;
    const normal = normalize(
      family.normalWeighted[0],
      family.normalWeighted[1],
      family.normalWeighted[2],
    );
    if (normal === null) continue;
    const offset = write * 3;
    positions[offset] = family.positionWeighted[0] / family.weight;
    positions[offset + 1] = family.positionWeighted[1] / family.weight;
    positions[offset + 2] = family.positionWeighted[2] / family.weight;
    normals[offset] = normal[0];
    normals[offset + 1] = normal[1];
    normals[offset + 2] = normal[2];
    const meanConfidence = family.confidenceWeighted / family.weight;
    const supportBonus = Math.min(0.08, Math.log2(family.members + 1) * 0.025);
    confidence[write] = clamp01(meanConfidence + supportBonus);
    write += 1;
  }

  return {
    positions: write === families.length ? positions : positions.subarray(0, write * 3),
    normals: write === families.length ? normals : normals.subarray(0, write * 3),
    confidence: write === families.length ? confidence : confidence.subarray(0, write),
    pointCount: write,
    keyframeCount: source.keyframeCount,
    voxelSizeMeters: source.voxelSizeMeters,
  };
}

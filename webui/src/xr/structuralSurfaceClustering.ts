import type {
  StructuralGuidePointCloudSnapshot,
  ProjectPoint,
} from './depthPointCloudV2';
import {
  estimateSurfaceFeatureComplexity,
  type SurfaceNormalEvidence,
} from './surfaceFeatureDensity';

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

interface SourceSample extends SurfaceNormalEvidence {
  index: number;
  position: ProjectPoint;
  normal: Vector3;
  confidence: number;
  baseCellKey: string;
}

const MAX_FAMILIES_PER_CELL = 3;
const FLAT_FAMILY_NORMAL_DOT = Math.cos((38 * Math.PI) / 180);
const FEATURE_FAMILY_NORMAL_DOT = Math.cos((24 * Math.PI) / 180);
const MIN_CLUSTER_CONFIDENCE = 0.42;
const FEATURE_DETAIL_THRESHOLD = 0.20;
const FEATURE_CELL_SCALE = 0.58;
const MIN_FEATURE_CELL_METERS = 0.028;
const MAX_COMPLEXITY_EVIDENCE = 96;

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
function cellCoordinates(point: ProjectPoint, size: number): readonly [number, number, number] {
  return [Math.floor(point[0] / size), Math.floor(point[1] / size), Math.floor(point[2] / size)];
}
function coordinateKey(x: number, y: number, z: number): string { return `${x}:${y}:${z}`; }
function cellKey(point: ProjectPoint, size: number): string {
  const [x, y, z] = cellCoordinates(point, size);
  return coordinateKey(x, y, z);
}
function parseCoordinateKey(key: string): readonly [number, number, number] | null {
  const values = key.split(':');
  if (values.length !== 3) return null;
  const x = Number(values[0]), y = Number(values[1]), z = Number(values[2]);
  return Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z) ? [x, y, z] : null;
}

function boundedNeighbourhoodEvidence(
  key: string,
  evidenceByCell: ReadonlyMap<string, readonly SurfaceNormalEvidence[]>,
): readonly SurfaceNormalEvidence[] {
  const coordinate = parseCoordinateKey(key);
  if (coordinate === null) return evidenceByCell.get(key) ?? [];
  const combined: SurfaceNormalEvidence[] = [];
  for (let dx = -1; dx <= 1; dx += 1) {
    for (let dy = -1; dy <= 1; dy += 1) {
      for (let dz = -1; dz <= 1; dz += 1) {
        const evidence = evidenceByCell.get(coordinateKey(
          coordinate[0] + dx,
          coordinate[1] + dy,
          coordinate[2] + dz,
        ));
        if (evidence !== undefined) combined.push(...evidence);
      }
    }
  }
  if (combined.length <= MAX_COMPLEXITY_EVIDENCE) return combined;

  // Structural inference already runs off the XR render path, but keep feature
  // classification bounded even in unusually dense local scans. A deterministic
  // stride samples the whole 3x3x3 neighbourhood instead of letting the first
  // spatial bucket dominate the normal distribution.
  const sampled = new Array<SurfaceNormalEvidence>(MAX_COMPLEXITY_EVIDENCE);
  const stride = combined.length / MAX_COMPLEXITY_EVIDENCE;
  for (let index = 0; index < MAX_COMPLEXITY_EVIDENCE; index += 1) {
    sampled[index] = combined[Math.min(combined.length - 1, Math.floor((index + 0.5) * stride))];
  }
  return sampled;
}

/**
 * Collapse promoted surfels into orientation families without flattening the
 * very geometry the structural guide needs most.
 *
 * Flat regions use coarse cells and a tolerant normal family. Cells that show
 * robust normal variation (smooth convex curvature or multiple supported
 * surfaces) are subdivided and use a tighter family angle. Complexity is
 * evaluated over the neighbouring 3x3x3 base cells so a real edge that happens
 * to lie exactly on a hash-cell boundary cannot disappear simply because each
 * side looked locally flat in isolation. The neighbourhood sample is bounded.
 *
 * This preserves more structural samples around bends/edges/corners while
 * leaving physical acceptance, multi-view promotion and the global memory
 * budget untouched. Opposite normal signs still belong to the same physical
 * surface family.
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

  const baseCellSize = Math.max(0.045, Math.min(0.075, source.voxelSizeMeters * 1.35));
  const featureCellSize = Math.max(MIN_FEATURE_CELL_METERS, baseCellSize * FEATURE_CELL_SCALE);
  const samples: SourceSample[] = [];
  const normalEvidenceByBaseCell = new Map<string, SurfaceNormalEvidence[]>();

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
    const baseCellKey = cellKey(position, baseCellSize);
    const sample: SourceSample = {
      index,
      position,
      normal,
      confidence,
      normalX: normal[0],
      normalY: normal[1],
      normalZ: normal[2],
      normalConfidence: confidence,
      baseCellKey,
    };
    samples.push(sample);
    const evidence = normalEvidenceByBaseCell.get(baseCellKey) ?? [];
    evidence.push(sample);
    normalEvidenceByBaseCell.set(baseCellKey, evidence);
  }
  if (samples.length === 0) return source;

  const featureScoreByBaseCell = new Map<string, number>();
  for (const key of normalEvidenceByBaseCell.keys()) {
    const neighbourhood = boundedNeighbourhoodEvidence(key, normalEvidenceByBaseCell);
    featureScoreByBaseCell.set(key, estimateSurfaceFeatureComplexity(neighbourhood).score);
  }

  const cells = new Map<string, Family[]>();
  for (const sample of samples) {
    const featureScore = featureScoreByBaseCell.get(sample.baseCellKey) ?? 0;
    const featureRich = featureScore >= FEATURE_DETAIL_THRESHOLD;
    const localCellKey = featureRich
      ? `${sample.baseCellKey}|feature:${cellKey(sample.position, featureCellSize)}`
      : `${sample.baseCellKey}|flat`;
    const familyNormalDot = featureRich ? FEATURE_FAMILY_NORMAL_DOT : FLAT_FAMILY_NORMAL_DOT;
    const families = cells.get(localCellKey) ?? [];

    let best: Family | null = null;
    let bestDot = familyNormalDot;
    for (const family of families) {
      const similarity = Math.abs(dot(family.referenceNormal, sample.normal));
      if (similarity >= bestDot) { best = family; bestDot = similarity; }
    }

    if (best === null) {
      if (families.length >= MAX_FAMILIES_PER_CELL) {
        // A fourth conflicting orientation in one tiny feature cell is more
        // likely mixed/flying evidence than a real fourth manifold.
        continue;
      }
      const created: Family = {
        cellKey: localCellKey,
        weight: sample.confidence,
        confidenceWeighted: sample.confidence * sample.confidence,
        positionWeighted: [
          sample.position[0] * sample.confidence,
          sample.position[1] * sample.confidence,
          sample.position[2] * sample.confidence,
        ],
        normalWeighted: [
          sample.normal[0] * sample.confidence,
          sample.normal[1] * sample.confidence,
          sample.normal[2] * sample.confidence,
        ],
        referenceNormal: sample.normal,
        members: 1,
      };
      families.push(created);
      cells.set(localCellKey, families);
      continue;
    }

    const aligned: Vector3 = dot(best.referenceNormal, sample.normal) < 0
      ? [-sample.normal[0], -sample.normal[1], -sample.normal[2]]
      : sample.normal;
    best.weight += sample.confidence;
    best.confidenceWeighted += sample.confidence * sample.confidence;
    best.positionWeighted[0] += sample.position[0] * sample.confidence;
    best.positionWeighted[1] += sample.position[1] * sample.confidence;
    best.positionWeighted[2] += sample.position[2] * sample.confidence;
    best.normalWeighted[0] += aligned[0] * sample.confidence;
    best.normalWeighted[1] += aligned[1] * sample.confidence;
    best.normalWeighted[2] += aligned[2] * sample.confidence;
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

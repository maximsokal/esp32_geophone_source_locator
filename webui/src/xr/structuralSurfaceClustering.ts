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
const FEATURE_DETAIL_THRESHOLD = 0.16;
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
function compareVector(a: Vector3, b: Vector3): number {
  if (a[0] !== b[0]) return a[0] - b[0];
  if (a[1] !== b[1]) return a[1] - b[1];
  return a[2] - b[2];
}
function compareNormalEvidence(a: SurfaceNormalEvidence, b: SurfaceNormalEvidence): number {
  if (a.normalConfidence !== b.normalConfidence) return b.normalConfidence - a.normalConfidence;
  const an = normalize(a.normalX, a.normalY, a.normalZ);
  const bn = normalize(b.normalX, b.normalY, b.normalZ);
  if (an === null) return bn === null ? 0 : 1;
  if (bn === null) return -1;
  const normalOrder = compareVector(canonical(an), canonical(bn));
  if (normalOrder !== 0) return normalOrder;
  // When bounded neighbourhoods contain many identical normals, position is the
  // deterministic tiebreaker. Otherwise Array.sort stability would preserve the
  // incoming camera-dependent order and the 96-point stride could sample a
  // different physical region from an equivalent permuted cloud.
  const ax = Number(a.x), ay = Number(a.y), az = Number(a.z);
  const bx = Number(b.x), by = Number(b.y), bz = Number(b.z);
  if (Number.isFinite(ax) && Number.isFinite(bx) && ax !== bx) return ax - bx;
  if (Number.isFinite(ay) && Number.isFinite(by) && ay !== by) return ay - by;
  if (Number.isFinite(az) && Number.isFinite(bz) && az !== bz) return az - bz;
  return 0;
}
function compareSourceSamples(a: SourceSample, b: SourceSample): number {
  if (a.confidence !== b.confidence) return b.confidence - a.confidence;
  if (a.position[0] !== b.position[0]) return a.position[0] - b.position[0];
  if (a.position[1] !== b.position[1]) return a.position[1] - b.position[1];
  if (a.position[2] !== b.position[2]) return a.position[2] - b.position[2];
  return compareVector(a.normal, b.normal);
}

function boundedNeighbourhoodEvidence(
  key: string,
  evidenceByCell: ReadonlyMap<string, readonly SurfaceNormalEvidence[]>,
): readonly SurfaceNormalEvidence[] {
  const coordinate = parseCoordinateKey(key);
  if (coordinate === null) return [...(evidenceByCell.get(key) ?? [])].sort(compareNormalEvidence);
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
  combined.sort(compareNormalEvidence);
  if (combined.length <= MAX_COMPLEXITY_EVIDENCE) return combined;

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
 * Metric positions are carried into the complexity estimator, so a few
 * conflicting normals in one tiny physical spot no longer trigger the same
 * refinement as a bend/edge/corner supported over measurable extent.
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
      x: position[0],
      y: position[1],
      z: position[2],
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
  samples.sort(compareSourceSamples);

  const featureScoreByBaseCell = new Map<string, number>();
  const baseCellKeys = [...normalEvidenceByBaseCell.keys()].sort();
  for (const key of baseCellKeys) {
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
      if (families.length >= MAX_FAMILIES_PER_CELL) continue;
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

  const families = [...cells.entries()]
    .sort(([left], [right]) => left.localeCompare(right))
    .flatMap(([, value]) => value);
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

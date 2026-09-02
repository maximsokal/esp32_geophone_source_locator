import {
  distance3,
  extractRobustPlanes,
  intersectTwoRobustPlanes,
  solveThreeRobustPlanes,
  type RobustPlane,
  type RobustPlaneSample,
} from './robustPlaneGeometry';

export type ProjectPoint = readonly [number, number, number];
export type ProjectVector = readonly [number, number, number];

export interface ManualPlacementSurfaceSnapshot {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  pointCount: number;
  voxelSizeMeters: number;
  /** Optional uncertainty/evidence channels. Older snapshots remain supported. */
  independentViews?: Uint8Array | Uint16Array;
  residualMeters?: Float32Array;
  normalConfidence?: Float32Array;
  maxViewAngleRadians?: Float32Array;
  fusionWeight?: Float32Array;
  coverageConfidence?: Float32Array;
  coverageReady?: Uint8Array;
}

export type ManualPlacementMode = 'surface' | 'edge' | 'corner';

export interface ManualPlacementResolution {
  accepted: boolean;
  position: ProjectPoint | null;
  mode: ManualPlacementMode | null;
  confidence: number;
  supportCount: number;
  sourceDistanceMeters: number | null;
  message: string;
  uncertaintyMeters?: number | null;
  conditionNumber?: number | null;
  /** Weakest robust measured arm among the three supporting corner planes. */
  minimumCornerArmSpanMeters?: number | null;
}

export type ManualPlacementResolver = (candidate: ProjectPoint) => ManualPlacementResolution;

interface LocalSample extends RobustPlaneSample {
  sourceIndex: number;
  distance: number;
}

interface CornerArmSupport {
  accepted: boolean;
  minimumSpanMeters: number;
}

let activeResolver: ManualPlacementResolver | null = null;

const MIN_SAMPLE_CONFIDENCE = 0.62;
const MAX_LOCAL_SAMPLES = 96;
const MIN_LOCAL_SAMPLES = 12;
const MIN_PLANE_SUPPORT = 6;
const MAX_EDGE_UNCERTAINTY_METERS = 0.035;
const MAX_CORNER_UNCERTAINTY_METERS = 0.032;
const MAX_CORNER_CONDITION_NUMBER = 35;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

function reject(
  message: string,
  supportCount: number,
  sourceDistanceMeters: number | null,
  uncertaintyMeters: number | null = null,
  conditionNumber: number | null = null,
): ManualPlacementResolution {
  return {
    accepted: false,
    position: null,
    mode: null,
    confidence: 0,
    supportCount,
    sourceDistanceMeters,
    message,
    uncertaintyMeters,
    conditionNumber,
    minimumCornerArmSpanMeters: null,
  };
}

function optionalAt(array: ArrayLike<number> | undefined, index: number, fallback: number): number {
  if (array === undefined || index >= array.length) return fallback;
  const value = Number(array[index]);
  return Number.isFinite(value) ? value : fallback;
}

function sampleEvidenceWeight(snapshot: ManualPlacementSurfaceSnapshot, index: number): number {
  const confidence = snapshot.confidence[index];
  const independentViews = optionalAt(snapshot.independentViews, index, 2);
  const residual = optionalAt(snapshot.residualMeters, index, 0.012);
  const normalConfidence = optionalAt(snapshot.normalConfidence, index, 0.65);
  const viewAngle = optionalAt(snapshot.maxViewAngleRadians, index, (10 * Math.PI) / 180);
  const fusionWeight = optionalAt(snapshot.fusionWeight, index, 2);
  const coverageConfidence = optionalAt(snapshot.coverageConfidence, index, confidence);
  const ready = optionalAt(snapshot.coverageReady, index, 1) > 0;
  if (!ready) return 0;
  if (independentViews < 2 || residual > 0.040 || normalConfidence < 0.22) return 0;

  const viewScore = clamp01((independentViews - 1) / 2);
  const angleScore = clamp01(viewAngle / ((18 * Math.PI) / 180));
  const residualScore = clamp01((0.040 - residual) / 0.034);
  const fusionScore = clamp01(fusionWeight / 4);
  return clamp01(
    confidence * 0.28 +
    coverageConfidence * 0.18 +
    viewScore * 0.18 +
    angleScore * 0.10 +
    residualScore * 0.16 +
    normalConfidence * 0.07 +
    fusionScore * 0.03,
  );
}

function planeSupportNear(
  plane: RobustPlane,
  samples: readonly LocalSample[],
  position: ProjectPoint,
  gateMeters: number,
): boolean {
  let nearest = Number.POSITIVE_INFINITY;
  for (const localIndex of plane.inlierIndices) {
    const sample = samples[localIndex];
    if (sample === undefined) continue;
    nearest = Math.min(nearest, distance3(sample.position, position));
  }
  return nearest <= gateMeters;
}

function robustQuantile(values: number[], q: number): number {
  if (values.length === 0) return 0;
  values.sort((a, b) => a - b);
  const position = clamp(q, 0, 1) * (values.length - 1);
  const lower = Math.floor(position);
  const upper = Math.ceil(position);
  if (lower === upper) return values[lower];
  const fraction = position - lower;
  return values[lower] * (1 - fraction) + values[upper] * fraction;
}

/**
 * A mathematically good three-plane intersection is not automatically a useful
 * structural corner. Each measured surface must continue a meaningful distance
 * away from the proposed vertex and must have several inliers along that arm.
 * This prevents a 2-3 cm chip/notch from being promoted like a building/furniture
 * corner merely because the three tiny patches happen to be well conditioned.
 */
function cornerArmSupport(
  planes: readonly [RobustPlane, RobustPlane, RobustPlane],
  samples: readonly LocalSample[],
  corner: ProjectPoint,
  voxelSizeMeters: number,
): CornerArmSupport {
  const minimumRequiredSpan = clamp(Math.abs(voxelSizeMeters) * 1.45, 0.055, 0.085);
  let minimumSpanMeters = Number.POSITIVE_INFINITY;
  for (const plane of planes) {
    const distances: number[] = [];
    for (const localIndex of plane.inlierIndices) {
      const sample = samples[localIndex];
      if (sample === undefined) continue;
      const value = distance3(sample.position, corner);
      if (Number.isFinite(value)) distances.push(value);
    }
    if (distances.length < MIN_PLANE_SUPPORT) return { accepted: false, minimumSpanMeters: 0 };
    const span = robustQuantile(distances, 0.80);
    minimumSpanMeters = Math.min(minimumSpanMeters, span);
    const farSupport = distances.filter((value) => value >= minimumRequiredSpan * 0.70).length;
    const requiredFarSupport = Math.max(3, Math.ceil(distances.length * 0.22));
    if (span < minimumRequiredSpan || farSupport < requiredFarSupport) {
      return { accepted: false, minimumSpanMeters: Number.isFinite(minimumSpanMeters) ? minimumSpanMeters : 0 };
    }
  }
  return {
    accepted: true,
    minimumSpanMeters: Number.isFinite(minimumSpanMeters) ? minimumSpanMeters : 0,
  };
}

interface SignedLineSupport {
  positive: number[];
  negative: number[];
}

function signedLineSupport(
  plane: RobustPlane,
  samples: readonly LocalSample[],
  origin: ProjectPoint,
  direction: ProjectVector,
  lineGateMeters: number,
): SignedLineSupport {
  const positive: number[] = [];
  const negative: number[] = [];
  for (const localIndex of plane.inlierIndices) {
    const sample = samples[localIndex];
    if (sample === undefined) continue;
    const dx = sample.position[0] - origin[0];
    const dy = sample.position[1] - origin[1];
    const dz = sample.position[2] - origin[2];
    const t = dx * direction[0] + dy * direction[1] + dz * direction[2];
    const closest: ProjectPoint = [
      origin[0] + direction[0] * t,
      origin[1] + direction[1] * t,
      origin[2] + direction[2] * t,
    ];
    if (distance3(sample.position, closest) > lineGateMeters) continue;
    if (t >= 0) positive.push(t);
    else negative.push(-t);
  }
  return { positive, negative };
}

function robustLineSpan(values: readonly number[]): number {
  return values.length < 2 ? 0 : robustQuantile([...values], 0.80);
}

/**
 * Three finite surface patches can individually pass the corner-arm test while
 * still failing to share the SAME physical edges. For every pair of planes we
 * therefore require measured inliers from BOTH planes near their intersection
 * line and on the same side of the solved vertex. Opposite-only support (+t on
 * one surface, -t on the other) is a mathematical crossing, not a measured edge.
 * The perpendicular gate deliberately tolerates a small physical depth hole at
 * the vertex/edge, but never invents longitudinal support across that hole.
 */
function cornerPairEdgeSupport(
  planes: readonly [RobustPlane, RobustPlane, RobustPlane],
  samples: readonly LocalSample[],
  corner: ProjectPoint,
  voxelSizeMeters: number,
): CornerArmSupport {
  const lineGateMeters = clamp(Math.abs(voxelSizeMeters) * 1.75, 0.055, 0.085);
  const minimumRequiredSpan = clamp(Math.abs(voxelSizeMeters) * 1.35, 0.050, 0.080);
  const pairs = [[0, 1], [0, 2], [1, 2]] as const;
  let minimumSpanMeters = Number.POSITIVE_INFINITY;

  for (const [firstIndex, secondIndex] of pairs) {
    const first = planes[firstIndex];
    const second = planes[secondIndex];
    const edge = intersectTwoRobustPlanes(first, second, corner);
    if (edge === null || edge.uncertaintyMeters > MAX_EDGE_UNCERTAINTY_METERS) {
      return { accepted: false, minimumSpanMeters: 0 };
    }
    if (distance3(edge.point, corner) > lineGateMeters) {
      return { accepted: false, minimumSpanMeters: 0 };
    }

    const firstSupport = signedLineSupport(first, samples, corner, edge.direction, lineGateMeters);
    const secondSupport = signedLineSupport(second, samples, corner, edge.direction, lineGateMeters);
    const positiveSpan = Math.min(
      robustLineSpan(firstSupport.positive),
      robustLineSpan(secondSupport.positive),
    );
    const negativeSpan = Math.min(
      robustLineSpan(firstSupport.negative),
      robustLineSpan(secondSupport.negative),
    );
    const usePositive = positiveSpan >= negativeSpan;
    const commonSpan = usePositive ? positiveSpan : negativeSpan;
    const firstSide = usePositive ? firstSupport.positive : firstSupport.negative;
    const secondSide = usePositive ? secondSupport.positive : secondSupport.negative;
    const usefulGate = minimumRequiredSpan * 0.55;
    const firstUseful = firstSide.filter((value) => value >= usefulGate).length;
    const secondUseful = secondSide.filter((value) => value >= usefulGate).length;

    if (commonSpan < minimumRequiredSpan || firstUseful < 2 || secondUseful < 2) {
      return {
        accepted: false,
        minimumSpanMeters: Number.isFinite(minimumSpanMeters) ? Math.min(minimumSpanMeters, commonSpan) : commonSpan,
      };
    }
    minimumSpanMeters = Math.min(minimumSpanMeters, commonSpan);
  }

  return {
    accepted: true,
    minimumSpanMeters: Number.isFinite(minimumSpanMeters) ? minimumSpanMeters : 0,
  };
}

function bestEdge(
  planes: readonly RobustPlane[],
  candidate: ProjectPoint,
): { position: ProjectPoint; uncertainty: number; confidence: number; support: number } | null {
  let best: { position: ProjectPoint; uncertainty: number; confidence: number; support: number; score: number } | null = null;
  for (let first = 0; first < planes.length; first += 1) {
    for (let second = first + 1; second < planes.length; second += 1) {
      const edge = intersectTwoRobustPlanes(planes[first], planes[second], candidate);
      if (edge === null || edge.uncertaintyMeters > MAX_EDGE_UNCERTAINTY_METERS) continue;
      const displacement = distance3(edge.point, candidate);
      const score = displacement + edge.uncertaintyMeters * 2.2 - edge.confidence * 0.025;
      if (best === null || score < best.score) {
        best = {
          position: edge.point,
          uncertainty: edge.uncertaintyMeters,
          confidence: edge.confidence,
          support: planes[first].support + planes[second].support,
          score,
        };
      }
    }
  }
  return best;
}

function compareLocalSamples(left: LocalSample, right: LocalSample): number {
  return left.distance - right.distance ||
    left.position[0] - right.position[0] ||
    left.position[1] - right.position[1] ||
    left.position[2] - right.position[2] ||
    Number(left.normal?.[0] ?? 0) - Number(right.normal?.[0] ?? 0) ||
    Number(left.normal?.[1] ?? 0) - Number(right.normal?.[1] ?? 0) ||
    Number(left.normal?.[2] ?? 0) - Number(right.normal?.[2] ?? 0) ||
    right.weight - left.weight;
}

export function solveManualPlacementOnStableSurface(
  candidate: ProjectPoint,
  snapshot: ManualPlacementSurfaceSnapshot,
): ManualPlacementResolution {
  const count = Math.min(
    snapshot.pointCount,
    Math.floor(snapshot.positions.length / 3),
    Math.floor(snapshot.normals.length / 3),
    snapshot.confidence.length,
  );
  if (count < MIN_LOCAL_SAMPLES) {
    return reject('SCAN MORE · not enough stable 3D support here.', count, null);
  }

  // Keep the neighbourhood local enough that a bed edge cannot borrow the floor
  // or a remote wall simply because their projections overlap in the image.
  const searchRadius = clamp(snapshot.voxelSizeMeters * 3.8, 0.10, 0.17);
  const sourceGate = clamp(snapshot.voxelSizeMeters * 2.2, 0.055, 0.095);
  const local: LocalSample[] = [];
  let nearestDistance = Number.POSITIVE_INFINITY;

  for (let index = 0; index < count; index += 1) {
    const confidence = snapshot.confidence[index];
    if (!Number.isFinite(confidence) || confidence < MIN_SAMPLE_CONFIDENCE) continue;
    const offset = index * 3;
    const position: ProjectPoint = [
      snapshot.positions[offset],
      snapshot.positions[offset + 1],
      snapshot.positions[offset + 2],
    ];
    const sampleDistance = distance3(candidate, position);
    nearestDistance = Math.min(nearestDistance, sampleDistance);
    if (sampleDistance > searchRadius) continue;
    const evidenceWeight = sampleEvidenceWeight(snapshot, index);
    if (evidenceWeight <= 0.05) continue;
    local.push({
      sourceIndex: index,
      position,
      normal: [
        snapshot.normals[offset],
        snapshot.normals[offset + 1],
        snapshot.normals[offset + 2],
      ],
      weight: evidenceWeight,
      residualMeters: optionalAt(snapshot.residualMeters, index, 0.012),
      normalConfidence: optionalAt(snapshot.normalConfidence, index, 0.65),
      distance: sampleDistance,
    });
  }

  local.sort(compareLocalSamples);
  const samples = local.slice(0, MAX_LOCAL_SAMPLES);
  if (samples.length < MIN_LOCAL_SAMPLES || nearestDistance > sourceGate) {
    return reject(
      'SCAN MORE · cursor is not supported by a dense stable surface neighbourhood.',
      samples.length,
      Number.isFinite(nearestDistance) ? nearestDistance : null,
    );
  }

  const inlierThreshold = clamp(snapshot.voxelSizeMeters * 0.38, 0.010, 0.019);
  const planes = extractRobustPlanes(samples, {
    maxPlanes: 3,
    minSupport: MIN_PLANE_SUPPORT,
    inlierThresholdMeters: inlierThreshold,
    normalGateRadians: (40 * Math.PI) / 180,
    minimumTangentSpreadMeters: clamp(snapshot.voxelSizeMeters * 0.48, 0.018, 0.030),
  });
  if (planes.length < 2) {
    return reject(
      'SCAN MORE · only one robust surface is supported here; observe the other side of the edge.',
      samples.length,
      nearestDistance,
    );
  }

  const supportNearGate = clamp(snapshot.voxelSizeMeters * 2.0, 0.055, 0.090);
  if (planes.length >= 3) {
    const cornerPlanes: [RobustPlane, RobustPlane, RobustPlane] = [planes[0], planes[1], planes[2]];
    const corner = solveThreeRobustPlanes(cornerPlanes[0], cornerPlanes[1], cornerPlanes[2]);
    const arms = corner === null
      ? { accepted: false, minimumSpanMeters: 0 }
      : cornerArmSupport(cornerPlanes, samples, corner.position, snapshot.voxelSizeMeters);
    const pairEdges = corner === null
      ? { accepted: false, minimumSpanMeters: 0 }
      : cornerPairEdgeSupport(cornerPlanes, samples, corner.position, snapshot.voxelSizeMeters);
    if (
      corner !== null &&
      arms.accepted &&
      pairEdges.accepted &&
      corner.conditionNumber <= MAX_CORNER_CONDITION_NUMBER &&
      corner.maxStdDevMeters <= MAX_CORNER_UNCERTAINTY_METERS &&
      distance3(corner.position, candidate) <= searchRadius &&
      cornerPlanes.every((plane) => planeSupportNear(plane, samples, corner.position, supportNearGate))
    ) {
      const planeQuality = Math.min(cornerPlanes[0].confidence, cornerPlanes[1].confidence, cornerPlanes[2].confidence);
      const uncertaintyScore = clamp01((MAX_CORNER_UNCERTAINTY_METERS - corner.maxStdDevMeters) / 0.026);
      const proximityScore = clamp01(1 - distance3(corner.position, candidate) / searchRadius);
      // Never advertise mathematically perfect certainty from consumer depth.
      const confidence = Math.min(0.97, clamp01(
        planeQuality * 0.42 + corner.confidence * 0.28 + uncertaintyScore * 0.20 + proximityScore * 0.10,
      ));
      if (confidence >= 0.68) {
        return {
          accepted: true,
          position: corner.position,
          mode: 'corner',
          confidence,
          supportCount: cornerPlanes[0].support + cornerPlanes[1].support + cornerPlanes[2].support,
          sourceDistanceMeters: nearestDistance,
          uncertaintyMeters: corner.maxStdDevMeters,
          conditionNumber: corner.conditionNumber,
          minimumCornerArmSpanMeters: Math.min(arms.minimumSpanMeters, pairEdges.minimumSpanMeters),
          message: 'CORNER LOCK · robust 3-plane intersection with finite measured pair edges.',
        };
      }
    }
  }

  const edge = bestEdge(planes, candidate);
  if (edge === null || distance3(edge.position, candidate) > searchRadius) {
    return reject(
      'SCAN MORE · local planes do not produce a well-conditioned edge near the cursor.',
      samples.length,
      nearestDistance,
    );
  }
  const supportingPlanes = planes.filter((plane) => planeSupportNear(plane, samples, edge.position, supportNearGate));
  if (supportingPlanes.length < 2) {
    return reject(
      'SCAN MORE · the mathematical plane intersection is outside measured surface support.',
      samples.length,
      nearestDistance,
      edge.uncertainty,
    );
  }
  const uncertaintyScore = clamp01((MAX_EDGE_UNCERTAINTY_METERS - edge.uncertainty) / 0.030);
  const proximityScore = clamp01(1 - distance3(edge.position, candidate) / searchRadius);
  const confidence = Math.min(0.95, clamp01(
    edge.confidence * 0.52 + uncertaintyScore * 0.30 + proximityScore * 0.18,
  ));
  if (confidence < 0.64) {
    return reject(
      'SCAN MORE · edge uncertainty is still too high.',
      edge.support,
      nearestDistance,
      edge.uncertainty,
    );
  }
  return {
    accepted: true,
    position: edge.position,
    mode: 'edge',
    confidence,
    supportCount: edge.support,
    sourceDistanceMeters: nearestDistance,
    uncertaintyMeters: edge.uncertainty,
    conditionNumber: null,
    minimumCornerArmSpanMeters: null,
    message: 'EDGE LOCK · robust 2-plane intersection with bounded uncertainty.',
  };
}

export function registerManualPlacementResolver(resolver: ManualPlacementResolver | null): void {
  activeResolver = resolver;
}

export function resolveManualPlacementCandidate(candidate: ProjectPoint): ManualPlacementResolution | null {
  return activeResolver?.(candidate) ?? null;
}

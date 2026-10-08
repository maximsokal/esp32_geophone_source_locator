import type { ScanPointCloudSnapshot } from './webxrRuntime';
import {
  enumerateUnknownCandidatePositions,
  type OccupancyBounds,
} from './occupancyEvidence';
import { MAX_MANUAL_CONTROL_POINTS } from './manualControlPoints';
import { MAX_OPERATOR_EDGES } from './operatorEdges';
import {
  MAX_ANNOTATION_JSON_BYTES,
  MAX_SCAN_ARTIFACT_BYTES,
  serializedAnnotationBytes,
} from './scanArtifact';
import {
  buildContinuationSpatialHash,
  nearestContinuationPoint,
  type ContinuationSpatialPoint,
} from './continuationSpatialIndex';

export interface ScanContinuationDiagnostics {
  yawRadians: number;
  translation: readonly [number, number, number];
  inliers: number;
  overlapRatio: number;
  rmsMeters: number;
  baseSampleCount: number;
  extensionSampleCount: number;
  conditioning?: {
    horizontalBaselineMeters: number;
    horizontalMinorVarianceMeters2: number;
    horizontalConditionRatio: number;
  };
  secondBestMargin?: {
    inlierDifference: number;
    rmsDifferenceMeters: number;
    translationSeparationMeters: number;
    yawSeparationRadians: number;
  } | null;
  rejectionReason?: 'ambiguous_overlap' | 'insufficient_conditioning' | 'merged_budget_exceeded';
  mergedBudgetEstimate?: {
    estimatedBytes: number;
    limitBytes: number;
    arrayBytes: number;
    headerReserveBytes: number;
    families: Readonly<Record<string, number>>;
  };
  runtimeOperations?: {
    yawHypotheses: number;
    translationVotes: number;
    scoredHypotheses: number;
    refinedHypotheses: number;
    localDescriptorDistanceChecks: number;
    featureDescriptorComparisons: number;
    featureCandidatePairs: number;
    icpIterations: number;
    localYawCandidates: number;
    localYawIterations: number;
    spatialBucketLookups: number;
    candidateDistanceChecks: number;
    coarseScratchBytes: number;
    descriptorScratchBytes: number;
  };
  /** True when the bounded annotation budgets omitted lower-priority extension data. */
  annotationsTruncated?: boolean;
}

/**
 * Rigid transform in the PROJECT frame (Z up): yaw about +Z, then XYZ translation.
 * For a continuation it maps extension (frame B) points into the base frame A.
 */
export interface RigidTransform {
  yaw: number;
  tx: number;
  ty: number;
  tz: number;
}

export type ScanContinuationResult =
  | { ok: true; snapshot: ScanPointCloudSnapshot; diagnostics: ScanContinuationDiagnostics; extensionToBase: RigidTransform }
  | { ok: false; reason: string; diagnostics: ScanContinuationDiagnostics | null };

export type ScanContinuationPhase = 'registration' | 'stable' | 'occupancy' | 'tsdf';

export interface ScanContinuationHooks {
  onPhase?: (phase: ScanContinuationPhase) => void;
}

export interface ContinuationBudgetFamilyEstimate {
  inputCopyBytes: number;
  transformedIntermediateBytes: number;
  mergeScratchBytes: number;
  outputBytes: number;
  totalBytes: number;
}

export type ContinuationBudgetFamily =
  | 'stablePositions' | 'stableConfidence' | 'stableNormals' | 'stableNormalConfidence'
  | 'tentativePositions' | 'tentativeConfidence'
  | 'freeAirPositions' | 'occupiedSurfacePositions' | 'unknownCandidatePositions'
  | 'tsdfPositions' | 'tsdfNormals' | 'tsdfConfidence' | 'tsdfResidualMeters'
  | 'tsdfNormalConfidence' | 'tsdfCoverageConfidence' | 'tsdfCoverageReady'
  | 'annotationJson' | 'registrationScratch';

export interface ContinuationBudgetEstimate {
  estimatedBytes: number;
  limitBytes: number;
  families: Readonly<Record<ContinuationBudgetFamily, ContinuationBudgetFamilyEstimate>>;
}

interface Point3 extends ContinuationSpatialPoint {
  featureIndex: number;
}

interface Match {
  source: Point3;
  target: Point3;
  distanceSquared: number;
}

interface RegistrationCandidate {
  transform: RigidTransform;
  matches: Match[];
  rmsMeters: number;
  overlapRatio: number;
}

interface RegistrationWorkCounters {
  yawHypotheses: number;
  translationVotes: number;
  scoredHypotheses: number;
  refinedHypotheses: number;
  localDescriptorDistanceChecks: number;
  featureDescriptorComparisons: number;
  featureCandidatePairs: number;
  icpIterations: number;
  localYawCandidates: number;
  localYawIterations: number;
  bucketLookups: number;
  candidateDistanceChecks: number;
}

const SAMPLE_LIMIT = 900;
const YAW_STEP_RADIANS = Math.PI / 36; // 5 degrees.
const REGISTRATION_CELL_METERS = 0.12;
const REGISTRATION_MAX_DISTANCE_METERS = 0.18;
/*
 * Hash cells are twice the original point spacing. Since a valid correspondence
 * is at most 0.18 m away and cells are 0.24 m wide, its cell index differs by
 * at most one on each axis. Keep nearestPoint's radius derived from the actual
 * distance/cell ratio so tuning either constant cannot silently lose matches.
 */
const REGISTRATION_HASH_CELL_METERS = REGISTRATION_CELL_METERS * 2;
const REGISTRATION_MIN_INLIERS = 90;
const REGISTRATION_MIN_OVERLAP = 0.18;
const REGISTRATION_MAX_RMS_METERS = 0.11;
const REFINEMENT_ITERATIONS = 6;
const REGISTRATION_MIN_HORIZONTAL_BASELINE_METERS = 0.45;
const REGISTRATION_MIN_HORIZONTAL_MINOR_VARIANCE_METERS2 = 0.0025;
const REGISTRATION_MIN_HORIZONTAL_CONDITION_RATIO = 0.01;
const REGISTRATION_AMBIGUOUS_RMS_MARGIN_METERS = 0.05;
/*
 * Translation voting is bounded at 160 x 160 pairs for each of 72 yaw seeds.
 * A 65,536-slot typed-array hash stores the per-bin key/count/sums. At most 16
 * vote bins per yaw are scored; the best 12 plus one identity seed are refined,
 * then at most eight get a bounded local yaw sweep. Vote scratch is 2,723,840 B;
 * descriptor arrays add at most 28,800 B and at most 16 score candidates are retained.
 */
const COARSE_SAMPLE_LIMIT = 160;
const COARSE_TRANSLATION_CELL_METERS = 0.48;
const COARSE_TRANSLATION_Z_CELL_METERS = 0.12;
const COARSE_TOP_BINS_PER_YAW = 10;
const COARSE_MIN_VOTES = 4;
const COARSE_MAX_REFINED_HYPOTHESES = 12;
const MAX_RETAINED_REGISTRATION_CANDIDATES = 16;
const FEATURE_NEIGHBOR_COUNT = 4;
const FEATURE_MATCHES_PER_SOURCE = 4;
const FEATURE_DESCRIPTOR_MAX_ERROR = 0.16;
const FEATURE_TOP_BINS_PER_YAW = 6;
const FEATURE_DESCRIPTOR_SCRATCH_BYTES = 2 * SAMPLE_LIMIT * FEATURE_NEIGHBOR_COUNT * Float32Array.BYTES_PER_ELEMENT;
const SCAN_ARTIFACT_FIXED_HEADER_BYTES = 20;
const SCAN_ARTIFACT_METADATA_RESERVE_BYTES = 16 * 1024;
const UNKNOWN_ENUMERATION_MAX_CELLS = 120_000;
/** Peak continuation working set allowed before the worker is started (256 MiB). */
export const CONTINUATION_TRANSIENT_BUDGET_BYTES = 256 * 1024 * 1024;
const CONTINUATION_MAP_ENTRY_ESTIMATE_BYTES = 160;
const CONTINUATION_TENTATIVE_ENTRY_ESTIMATE_BYTES = 96;
/* Bounded vote table + feature descriptors + 1 MiB for sampled match objects and sorting overhead. */
const CONTINUATION_REGISTRATION_SCRATCH_BYTES = 2_723_840 + FEATURE_DESCRIPTOR_SCRATCH_BYTES + 1024 * 1024;
const LOCAL_YAW_SWEEP_DEGREES = 5;
const LOCAL_YAW_STEP_DEGREES = 1;
const LOCAL_YAW_TOP_HYPOTHESES = 8;
const LOCAL_YAW_ICP_ITERATIONS = 3;
const COARSE_VOTE_CAPACITY = 65_536;
const COARSE_PAIR_LIMIT = COARSE_SAMPLE_LIMIT * COARSE_SAMPLE_LIMIT;
const COARSE_SCRATCH_BYTES =
  COARSE_VOTE_CAPACITY * (3 * Int32Array.BYTES_PER_ELEMENT + Int32Array.BYTES_PER_ELEMENT + 3 * Float64Array.BYTES_PER_ELEMENT) +
  COARSE_PAIR_LIMIT * Int32Array.BYTES_PER_ELEMENT;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function pointCount(values: ArrayLike<number>): number {
  return Math.floor(values.length / 3);
}

/** Conservative peak live bytes across request copies, transformed arrays, merge maps and output arrays. */
export function estimateContinuationBudget(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  limitBytes = CONTINUATION_TRANSIENT_BUDGET_BYTES,
): ContinuationBudgetEstimate {
  const baseStable = pointCount(base.positions);
  const extensionStable = pointCount(extension.positions);
  const stable = baseStable + extensionStable;
  const baseTentative = pointCount(base.tentativePositions);
  const extensionTentative = pointCount(extension.tentativePositions);
  const tentative = baseTentative + extensionTentative;
  const tsdf = Math.max(base.tsdfPointCount, pointCount(base.tsdfPositions)) +
    Math.max(extension.tsdfPointCount, pointCount(extension.tsdfPositions));
  const annotationBaseBytes = serializedAnnotationBytes(base.manualControlPoints, base.operatorEdges);
  const annotationExtensionBytes = serializedAnnotationBytes(extension.manualControlPoints, extension.operatorEdges);
  const annotationTotalBytes = annotationBaseBytes + annotationExtensionBytes;
  const families = {} as Record<ContinuationBudgetFamily, ContinuationBudgetFamilyEstimate>;
  const addFamily = (
    name: ContinuationBudgetFamily,
    inputCopyBytes: number,
    transformedIntermediateBytes: number,
    mergeScratchBytes: number,
    outputBytes: number,
  ): void => {
    families[name] = {
      inputCopyBytes,
      transformedIntermediateBytes,
      mergeScratchBytes,
      outputBytes,
      totalBytes: inputCopyBytes + transformedIntermediateBytes + mergeScratchBytes + outputBytes,
    };
  };
  const inputBytes = (baseValues: ArrayBufferView, extensionValues: ArrayBufferView): number =>
    baseValues.byteLength + extensionValues.byteLength;

  addFamily('stablePositions', inputBytes(base.positions, extension.positions), extension.positions.byteLength,
    stable * CONTINUATION_MAP_ENTRY_ESTIMATE_BYTES + (stable + tentative) * 3 * Float32Array.BYTES_PER_ELEMENT,
    stable * 3 * Float32Array.BYTES_PER_ELEMENT);
  addFamily('stableConfidence', inputBytes(base.confidence, extension.confidence), 0, 0,
    stable * Float32Array.BYTES_PER_ELEMENT);
  addFamily('stableNormals', inputBytes(base.normals, extension.normals), extension.normals.byteLength, 0,
    stable * 3 * Float32Array.BYTES_PER_ELEMENT);
  addFamily('stableNormalConfidence', (base.normalConfidence?.byteLength ?? 0) + (extension.normalConfidence?.byteLength ?? 0), 0, 0,
    base.normalConfidence === undefined && extension.normalConfidence === undefined ? 0 : stable * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tentativePositions', inputBytes(base.tentativePositions, extension.tentativePositions), extension.tentativePositions.byteLength,
    tentative * CONTINUATION_TENTATIVE_ENTRY_ESTIMATE_BYTES, tentative * 3 * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tentativeConfidence', inputBytes(base.tentativeConfidence, extension.tentativeConfidence), 0, 0,
    tentative * Float32Array.BYTES_PER_ELEMENT);

  const occupancyFamily = (
    name: 'freeAirPositions' | 'occupiedSurfacePositions',
    baseValues: Float32Array,
    extensionValues: Float32Array,
  ): void => {
    const points = pointCount(baseValues) + pointCount(extensionValues);
    addFamily(name, inputBytes(baseValues, extensionValues), extensionValues.byteLength,
      points * CONTINUATION_MAP_ENTRY_ESTIMATE_BYTES, points * 3 * Float32Array.BYTES_PER_ELEMENT);
  };
  occupancyFamily('freeAirPositions', base.freeAirPositions, extension.freeAirPositions);
  occupancyFamily('occupiedSurfacePositions', base.occupiedSurfacePositions, extension.occupiedSurfacePositions);
  addFamily('unknownCandidatePositions', inputBytes(base.unknownCandidatePositions, extension.unknownCandidatePositions), 0,
    UNKNOWN_ENUMERATION_MAX_CELLS * CONTINUATION_MAP_ENTRY_ESTIMATE_BYTES,
    UNKNOWN_ENUMERATION_MAX_CELLS * 3 * Float32Array.BYTES_PER_ELEMENT);

  addFamily('tsdfPositions', inputBytes(base.tsdfPositions, extension.tsdfPositions), extension.tsdfPositions.byteLength,
    tsdf * CONTINUATION_MAP_ENTRY_ESTIMATE_BYTES, tsdf * 3 * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tsdfNormals', inputBytes(base.tsdfNormals, extension.tsdfNormals), extension.tsdfNormals.byteLength, 0,
    tsdf * 3 * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tsdfConfidence', inputBytes(base.tsdfConfidence, extension.tsdfConfidence), 0, 0, tsdf * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tsdfResidualMeters', inputBytes(base.tsdfResidualMeters, extension.tsdfResidualMeters), 0, 0, tsdf * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tsdfNormalConfidence', inputBytes(base.tsdfNormalConfidence, extension.tsdfNormalConfidence), 0, 0, tsdf * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tsdfCoverageConfidence', inputBytes(base.tsdfCoverageConfidence, extension.tsdfCoverageConfidence), 0, 0, tsdf * Float32Array.BYTES_PER_ELEMENT);
  addFamily('tsdfCoverageReady', inputBytes(base.tsdfCoverageReady, extension.tsdfCoverageReady), 0, 0, tsdf * Uint8Array.BYTES_PER_ELEMENT);
  addFamily('annotationJson', annotationTotalBytes, annotationExtensionBytes, 0,
    Math.min(MAX_ANNOTATION_JSON_BYTES, annotationTotalBytes));
  addFamily('registrationScratch', 0, 0, CONTINUATION_REGISTRATION_SCRATCH_BYTES, 0);

  const estimatedBytes = Object.values(families).reduce((sum, family) => sum + family.totalBytes, 0);
  return { estimatedBytes, limitBytes: Math.max(0, Math.floor(limitBytes)), families };
}

function reportContinuationPhase(hooks: ScanContinuationHooks | undefined, phase: ScanContinuationPhase): void {
  try { hooks?.onPhase?.(phase); }
  catch { /* Progress observers cannot alter the deterministic merge result. */ }
}

function samplePositions(values: Float32Array, limit = SAMPLE_LIMIT): Point3[] {
  const count = pointCount(values);
  if (count === 0) return [];
  const take = Math.min(count, limit);
  const result: Point3[] = [];
  for (let sample = 0; sample < take; sample += 1) {
    const index = Math.min(count - 1, Math.floor((sample * count) / take));
    const offset = index * 3;
    const x = Number(values[offset]);
    const y = Number(values[offset + 1]);
    const z = Number(values[offset + 2]);
    if (finite(x) && finite(y) && finite(z)) {
      result.push({ x, y, z, sourceIndex: index, featureIndex: result.length });
    }
  }
  return result;
}

function transformPoint(point: Point3, transform: RigidTransform): Point3 {
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  return {
    x: c * point.x - s * point.y + transform.tx,
    y: s * point.x + c * point.y + transform.ty,
    z: point.z + transform.tz,
    sourceIndex: point.sourceIndex,
    featureIndex: point.featureIndex,
  };
}

function voxelCoordinate(value: number, size: number): number {
  return Math.floor(value / size);
}

function voxelKey(x: number, y: number, z: number, size: number): string {
  return `${voxelCoordinate(x, size)}:${voxelCoordinate(y, size)}:${voxelCoordinate(z, size)}`;
}

function fineOccupancyVoxelSize(snapshot: ScanPointCloudSnapshot): number {
  return snapshot.occupancyFineVoxelSizeMeters ?? snapshot.occupancyVoxelSizeMeters;
}

function collectMatches(
  source: readonly Point3[],
  targetHash: ReadonlyMap<string, Point3[]>,
  transform: RigidTransform,
  operations: RegistrationWorkCounters,
  features: RegistrationFeatureData,
): Match[] {
  /*
   * Keep at most one source correspondence per saved target point. Without this
   * constraint a compressed/distorted continuation can collapse many extension
   * samples onto the same few base samples and fake a high inlier/overlap score.
   * The smallest-residual source wins deterministically for each target.
   */
  const bestByTarget = new Map<number, Match>();
  for (const sourcePoint of source) {
    const transformed = transformPoint(sourcePoint, transform);
    const nearest = nearestContinuationPoint(
      transformed,
      targetHash,
      REGISTRATION_HASH_CELL_METERS,
      REGISTRATION_MAX_DISTANCE_METERS,
      operations,
      (candidate) => featureDescriptorError(sourcePoint, candidate, features) <= FEATURE_DESCRIPTOR_MAX_ERROR,
    );
    if (nearest === null) continue;
    const match: Match = {
      source: sourcePoint,
      target: nearest.point,
      distanceSquared: nearest.distanceSquared,
    };
    const previous = bestByTarget.get(nearest.point.sourceIndex);
    if (previous === undefined || match.distanceSquared < previous.distanceSquared) {
      bestByTarget.set(nearest.point.sourceIndex, match);
    }
  }
  return [...bestByTarget.values()];
}

function solveYawTranslation(matches: readonly Match[]): RigidTransform | null {
  if (matches.length < 3) return null;
  let sx = 0, sy = 0, sz = 0, tx = 0, ty = 0, tz = 0;
  for (const match of matches) {
    sx += match.source.x; sy += match.source.y; sz += match.source.z;
    tx += match.target.x; ty += match.target.y; tz += match.target.z;
  }
  const inv = 1 / matches.length;
  sx *= inv; sy *= inv; sz *= inv; tx *= inv; ty *= inv; tz *= inv;
  let a = 0, b = 0;
  for (const match of matches) {
    const xs = match.source.x - sx;
    const ys = match.source.y - sy;
    const xt = match.target.x - tx;
    const yt = match.target.y - ty;
    a += xs * xt + ys * yt;
    b += xs * yt - ys * xt;
  }
  if (Math.hypot(a, b) < 1e-12) return null;
  const yaw = Math.atan2(b, a);
  const c = Math.cos(yaw);
  const s = Math.sin(yaw);
  return {
    yaw,
    tx: tx - (c * sx - s * sy),
    ty: ty - (s * sx + c * sy),
    tz: tz - sz,
  };
}

function scoreTransform(
  source: readonly Point3[],
  targetHash: ReadonlyMap<string, Point3[]>,
  transform: RigidTransform,
  denominator: number,
  operations: RegistrationWorkCounters,
  features: RegistrationFeatureData,
): { matches: Match[]; rmsMeters: number; overlapRatio: number } {
  const matches = collectMatches(source, targetHash, transform, operations, features);
  const squared = matches.reduce((sum, match) => sum + match.distanceSquared, 0);
  return {
    matches,
    rmsMeters: matches.length > 0 ? Math.sqrt(squared / matches.length) : Number.POSITIVE_INFINITY,
    overlapRatio: matches.length / Math.max(1, denominator),
  };
}

function registerExtension(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
): { transform: RigidTransform; diagnostics: ScanContinuationDiagnostics } | null {
  const basePoints = samplePositions(base.positions);
  const extensionPoints = samplePositions(extension.positions);
  if (basePoints.length < 120 || extensionPoints.length < 120) return null;
  const targetHash = buildContinuationSpatialHash(basePoints, REGISTRATION_HASH_CELL_METERS);
  const coarseBasePoints = samplePositions(base.positions, COARSE_SAMPLE_LIMIT);
  const coarseExtensionPoints = samplePositions(extension.positions, COARSE_SAMPLE_LIMIT);
  const denominator = Math.min(basePoints.length, extensionPoints.length);
  const operations: RegistrationWorkCounters = {
    yawHypotheses: 0,
    translationVotes: 0,
    scoredHypotheses: 0,
    refinedHypotheses: 0,
    localDescriptorDistanceChecks: 0,
    featureDescriptorComparisons: 0,
    featureCandidatePairs: 0,
    icpIterations: 0,
    localYawCandidates: 0,
    localYawIterations: 0,
    bucketLookups: 0,
    candidateDistanceChecks: 0,
  };
  const features = featureCorrespondences(basePoints, extensionPoints, operations);
  const voteBuffer = createTranslationVoteBuffer();
  const candidates: RegistrationCandidate[] = [];
  const translationSeeds: Array<{ transform: RigidTransform; votes: number }> = [];

  /* Retain one identity seed for scans already expressed in the saved frame. */
  let identityTransform: RigidTransform = { yaw: 0, tx: 0, ty: 0, tz: 0 };
  operations.refinedHypotheses += 1;
  for (let iteration = 0; iteration < REFINEMENT_ITERATIONS; iteration += 1) {
    operations.icpIterations += 1;
    const matches = collectMatches(extensionPoints, targetHash, identityTransform, operations, features);
    const solved = solveYawTranslation(matches);
    if (solved === null) break;
    identityTransform = solved;
  }
  retainRegistrationCandidate(candidates, {
    transform: identityTransform,
    ...scoreTransform(extensionPoints, targetHash, identityTransform, denominator, operations, features),
  });

  for (let hypothesis = 0; hypothesis < Math.round((Math.PI * 2) / YAW_STEP_RADIANS); hypothesis += 1) {
    operations.yawHypotheses += 1;
    const yaw = hypothesis * YAW_STEP_RADIANS;
    const c = Math.cos(yaw);
    const s = Math.sin(yaw);
    for (const sourcePoint of coarseExtensionPoints) {
      const rotatedX = c * sourcePoint.x - s * sourcePoint.y;
      const rotatedY = s * sourcePoint.x + c * sourcePoint.y;
      for (const targetPoint of coarseBasePoints) {
        if (addTranslationVote(
          voteBuffer,
          targetPoint.x - rotatedX,
          targetPoint.y - rotatedY,
          targetPoint.z - sourcePoint.z,
        )) operations.translationVotes += 1;
      }
    }

    const topBins = topTranslationVoteBins(voteBuffer);
    for (const slot of topBins) {
      translationSeeds.push({
        transform: {
          yaw,
          tx: voteBuffer.sumX[slot] / voteBuffer.count[slot],
          ty: voteBuffer.sumY[slot] / voteBuffer.count[slot],
          tz: voteBuffer.sumZ[slot] / voteBuffer.count[slot],
        },
        votes: voteBuffer.count[slot],
      });
    }
    clearTranslationVoteBins(voteBuffer);

    for (const pair of features.pairs) {
      const rotatedX = c * pair.source.x - s * pair.source.y;
      const rotatedY = s * pair.source.x + c * pair.source.y;
      if (addTranslationVote(
        voteBuffer,
        pair.target.x - rotatedX,
        pair.target.y - rotatedY,
        pair.target.z - pair.source.z,
      )) operations.translationVotes += 1;
    }
    for (const slot of topTranslationVoteBins(voteBuffer).slice(0, FEATURE_TOP_BINS_PER_YAW)) {
      translationSeeds.push({
        transform: {
          yaw,
          tx: voteBuffer.sumX[slot] / voteBuffer.count[slot],
          ty: voteBuffer.sumY[slot] / voteBuffer.count[slot],
          tz: voteBuffer.sumZ[slot] / voteBuffer.count[slot],
        },
        votes: voteBuffer.count[slot],
      });
    }
    clearTranslationVoteBins(voteBuffer);
  }

  const scoredSeeds: Array<RegistrationCandidate & { votes: number }> = [];
  for (const seed of translationSeeds) {
    operations.scoredHypotheses += 1;
    const scored = {
      transform: seed.transform,
      votes: seed.votes,
      ...scoreTransform(extensionPoints, targetHash, seed.transform, denominator, operations, features),
    };
    let index = 0;
    while (index < scoredSeeds.length && (
      scoredSeeds[index].matches.length > scored.matches.length ||
      (scoredSeeds[index].matches.length === scored.matches.length && (
        scoredSeeds[index].rmsMeters < scored.rmsMeters ||
        (scoredSeeds[index].rmsMeters === scored.rmsMeters && scoredSeeds[index].votes >= scored.votes)
      ))
    )) index += 1;
    if (index >= COARSE_MAX_REFINED_HYPOTHESES) continue;
    scoredSeeds.splice(index, 0, scored);
    if (scoredSeeds.length > COARSE_MAX_REFINED_HYPOTHESES) scoredSeeds.pop();
  }
  for (const seed of scoredSeeds.slice(0, COARSE_MAX_REFINED_HYPOTHESES)) {
    let transform = seed.transform;
    operations.refinedHypotheses += 1;
    for (let iteration = 0; iteration < REFINEMENT_ITERATIONS; iteration += 1) {
      operations.icpIterations += 1;
      const matches = collectMatches(extensionPoints, targetHash, transform, operations, features);
      const solved = solveYawTranslation(matches);
      if (solved === null) break;
      transform = solved;
    }
    retainRegistrationCandidate(candidates, {
      transform,
      ...scoreTransform(extensionPoints, targetHash, transform, denominator, operations, features),
    });
  }

  candidates.sort(compareRegistrationCandidates);
  const candidatesForYawSweep = candidates.slice(0, LOCAL_YAW_TOP_HYPOTHESES);
  for (const candidate of candidatesForYawSweep) {
    for (let deltaDegrees = -LOCAL_YAW_SWEEP_DEGREES; deltaDegrees <= LOCAL_YAW_SWEEP_DEGREES; deltaDegrees += LOCAL_YAW_STEP_DEGREES) {
      const yaw = candidate.transform.yaw + deltaDegrees * Math.PI / 180;
      let transform = solveTranslationAtYaw(candidate.matches, yaw) ?? { ...candidate.transform, yaw };
      operations.localYawCandidates += 1;
      for (let iteration = 0; iteration < LOCAL_YAW_ICP_ITERATIONS; iteration += 1) {
        operations.localYawIterations += 1;
        const matches = collectMatches(extensionPoints, targetHash, transform, operations, features);
        const solved = solveTranslationAtYaw(matches, yaw);
        if (solved === null) break;
        transform = solved;
      }
      retainRegistrationCandidate(candidates, {
        transform,
        ...scoreTransform(extensionPoints, targetHash, transform, denominator, operations, features),
      });
    }
  }

  if (candidates.length === 0) return null;
  candidates.sort(compareRegistrationCandidates);
  const best = candidates[0];
  const secondBest = candidates.slice(1).find((candidate) => {
    const translationSeparation = Math.hypot(
      candidate.transform.tx - best.transform.tx,
      candidate.transform.ty - best.transform.ty,
      candidate.transform.tz - best.transform.tz,
    );
    return translationSeparation > 0.25 || yawSeparation(candidate.transform.yaw, best.transform.yaw) > 5 * Math.PI / 180;
  });
  const conditioning = horizontalConditioning(best.matches);
  const secondBestMargin = secondBest === undefined ? null : {
    inlierDifference: best.matches.length - secondBest.matches.length,
    rmsDifferenceMeters: secondBest.rmsMeters - best.rmsMeters,
    translationSeparationMeters: Math.hypot(
      secondBest.transform.tx - best.transform.tx,
      secondBest.transform.ty - best.transform.ty,
      secondBest.transform.tz - best.transform.tz,
    ),
    yawSeparationRadians: yawSeparation(secondBest.transform.yaw, best.transform.yaw),
  };
  const diagnostics: ScanContinuationDiagnostics = {
    yawRadians: best.transform.yaw,
    translation: [best.transform.tx, best.transform.ty, best.transform.tz],
    inliers: best.matches.length,
    overlapRatio: best.overlapRatio,
    rmsMeters: best.rmsMeters,
    baseSampleCount: basePoints.length,
    extensionSampleCount: extensionPoints.length,
    conditioning,
    secondBestMargin,
    runtimeOperations: {
      yawHypotheses: operations.yawHypotheses,
      translationVotes: operations.translationVotes,
      scoredHypotheses: operations.scoredHypotheses,
      refinedHypotheses: operations.refinedHypotheses,
      localDescriptorDistanceChecks: operations.localDescriptorDistanceChecks,
      featureDescriptorComparisons: operations.featureDescriptorComparisons,
      featureCandidatePairs: operations.featureCandidatePairs,
      icpIterations: operations.icpIterations,
      localYawCandidates: operations.localYawCandidates,
      localYawIterations: operations.localYawIterations,
      spatialBucketLookups: operations.bucketLookups,
      candidateDistanceChecks: operations.candidateDistanceChecks,
      coarseScratchBytes: COARSE_SCRATCH_BYTES,
      descriptorScratchBytes: FEATURE_DESCRIPTOR_SCRATCH_BYTES,
    },
  };
  if (
    conditioning.horizontalBaselineMeters < REGISTRATION_MIN_HORIZONTAL_BASELINE_METERS ||
    conditioning.horizontalMinorVarianceMeters2 < REGISTRATION_MIN_HORIZONTAL_MINOR_VARIANCE_METERS2 ||
    conditioning.horizontalConditionRatio < REGISTRATION_MIN_HORIZONTAL_CONDITION_RATIO
  ) diagnostics.rejectionReason = 'insufficient_conditioning';
  else if (
    secondBestMargin !== null &&
    secondBestMargin.inlierDifference >= 0 &&
    secondBestMargin.inlierDifference <= Math.max(3, Math.floor(best.matches.length * 0.05)) &&
    Math.abs(secondBestMargin.rmsDifferenceMeters) <= REGISTRATION_AMBIGUOUS_RMS_MARGIN_METERS
  ) diagnostics.rejectionReason = 'ambiguous_overlap';
  if (
    diagnostics.inliers < REGISTRATION_MIN_INLIERS ||
    diagnostics.overlapRatio < REGISTRATION_MIN_OVERLAP ||
    diagnostics.rmsMeters > REGISTRATION_MAX_RMS_METERS
  ) return { transform: best.transform, diagnostics };
  return { transform: best.transform, diagnostics };
}

function transformPositions(values: Float32Array, transform: RigidTransform): Float32Array {
  const result = new Float32Array(values.length);
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  for (let offset = 0; offset + 2 < values.length; offset += 3) {
    const x = values[offset];
    const y = values[offset + 1];
    result[offset] = c * x - s * y + transform.tx;
    result[offset + 1] = s * x + c * y + transform.ty;
    result[offset + 2] = values[offset + 2] + transform.tz;
  }
  return result;
}

function transformNormals(values: Float32Array, transform: RigidTransform): Float32Array {
  const result = new Float32Array(values.length);
  const c = Math.cos(transform.yaw);
  const s = Math.sin(transform.yaw);
  for (let offset = 0; offset + 2 < values.length; offset += 3) {
    const x = values[offset];
    const y = values[offset + 1];
    result[offset] = c * x - s * y;
    result[offset + 1] = s * x + c * y;
    result[offset + 2] = values[offset + 2];
  }
  return result;
}

function appendFloatArrays(a: Float32Array, b: Float32Array): Float32Array {
  const result = new Float32Array(a.length + b.length);
  result.set(a, 0);
  result.set(b, a.length);
  return result;
}

function mergeSurface(
  basePositions: Float32Array,
  baseConfidence: Float32Array,
  baseNormals: Float32Array,
  baseNormalConfidence: Float32Array | undefined,
  extensionPositions: Float32Array,
  extensionConfidence: Float32Array,
  extensionNormals: Float32Array,
  extensionNormalConfidence: Float32Array | undefined,
  voxelSizeMeters: number,
): { positions: Float32Array; confidence: Float32Array; normals: Float32Array; normalConfidence?: Float32Array } {
  /* Select one atomic sample per voxel: known normal, confidence, then base on ties. */
  interface SurfaceEvidence {
    key: string;
    x: number; y: number; z: number;
    nx: number; ny: number; nz: number;
    confidence: number;
    normalConfidence: number;
    sourceRank: 0 | 1;
  }
  const selected = new Map<string, SurfaceEvidence>();
  const hasNormalConfidence = baseNormalConfidence !== undefined || extensionNormalConfidence !== undefined;
  const isBetter = (candidate: SurfaceEvidence, previous: SurfaceEvidence): boolean => {
    const candidateKnownNormal = candidate.normalConfidence > 0;
    const previousKnownNormal = previous.normalConfidence > 0;
    if (candidateKnownNormal !== previousKnownNormal) return candidateKnownNormal;
    if (candidate.confidence !== previous.confidence) return candidate.confidence > previous.confidence;
    if (candidate.sourceRank !== previous.sourceRank) return candidate.sourceRank < previous.sourceRank;
    const candidateValues = [candidate.x, candidate.y, candidate.z, candidate.nx, candidate.ny, candidate.nz];
    const previousValues = [previous.x, previous.y, previous.z, previous.nx, previous.ny, previous.nz];
    for (let index = 0; index < candidateValues.length; index += 1) {
      if (candidateValues[index] !== previousValues[index]) return candidateValues[index] < previousValues[index];
    }
    return false;
  };
  const add = (
    p: Float32Array,
    c: Float32Array,
    n: Float32Array,
    nc: Float32Array | undefined,
    index: number,
    sourceRank: 0 | 1,
  ): void => {
    const offset = index * 3;
    const key = voxelKey(p[offset], p[offset + 1], p[offset + 2], voxelSizeMeters);
    const candidate: SurfaceEvidence = {
      key,
      x: p[offset], y: p[offset + 1], z: p[offset + 2],
      nx: n[offset] ?? 0, ny: n[offset + 1] ?? 0, nz: n[offset + 2] ?? 1,
      confidence: c[index] ?? 0,
      normalConfidence: nc?.[index] ?? 0,
      sourceRank,
    };
    const previous = selected.get(key);
    if (previous === undefined || isBetter(candidate, previous)) selected.set(key, candidate);
  };
  for (let index = 0; index < pointCount(basePositions); index += 1) {
    add(basePositions, baseConfidence, baseNormals, baseNormalConfidence, index, 0);
  }
  for (let index = 0; index < pointCount(extensionPositions); index += 1) {
    add(extensionPositions, extensionConfidence, extensionNormals, extensionNormalConfidence, index, 1);
  }
  const ordered = [...selected.values()].sort((a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0);
  const positions = new Float32Array(ordered.length * 3);
  const confidence = new Float32Array(ordered.length);
  const normals = new Float32Array(ordered.length * 3);
  const normalConfidence = hasNormalConfidence ? new Float32Array(ordered.length) : undefined;
  for (let index = 0; index < ordered.length; index += 1) {
    const evidence = ordered[index];
    const offset = index * 3;
    positions[offset] = evidence.x; positions[offset + 1] = evidence.y; positions[offset + 2] = evidence.z;
    confidence[index] = evidence.confidence;
    normals[offset] = evidence.nx; normals[offset + 1] = evidence.ny; normals[offset + 2] = evidence.nz;
    if (normalConfidence !== undefined) normalConfidence[index] = evidence.normalConfidence;
  }
  return {
    positions,
    confidence,
    normals,
    ...(normalConfidence === undefined ? {} : { normalConfidence }),
  };
}

function solveTranslationAtYaw(matches: readonly Match[], yaw: number): RigidTransform | null {
  if (matches.length < 3) return null;
  let sx = 0, sy = 0, sz = 0, tx = 0, ty = 0, tz = 0;
  for (const match of matches) {
    sx += match.source.x; sy += match.source.y; sz += match.source.z;
    tx += match.target.x; ty += match.target.y; tz += match.target.z;
  }
  const inverse = 1 / matches.length;
  sx *= inverse; sy *= inverse; sz *= inverse;
  tx *= inverse; ty *= inverse; tz *= inverse;
  const c = Math.cos(yaw);
  const s = Math.sin(yaw);
  return {
    yaw,
    tx: tx - (c * sx - s * sy),
    ty: ty - (s * sx + c * sy),
    tz: tz - sz,
  };
}

interface TranslationVoteBuffer {
  x: Int32Array;
  y: Int32Array;
  z: Int32Array;
  count: Int32Array;
  sumX: Float64Array;
  sumY: Float64Array;
  sumZ: Float64Array;
  usedSlots: Int32Array;
  usedCount: number;
}

function createTranslationVoteBuffer(): TranslationVoteBuffer {
  return {
    x: new Int32Array(COARSE_VOTE_CAPACITY),
    y: new Int32Array(COARSE_VOTE_CAPACITY),
    z: new Int32Array(COARSE_VOTE_CAPACITY),
    count: new Int32Array(COARSE_VOTE_CAPACITY),
    sumX: new Float64Array(COARSE_VOTE_CAPACITY),
    sumY: new Float64Array(COARSE_VOTE_CAPACITY),
    sumZ: new Float64Array(COARSE_VOTE_CAPACITY),
    usedSlots: new Int32Array(COARSE_PAIR_LIMIT),
    usedCount: 0,
  };
}

function translationCellHash(x: number, y: number, z: number): number {
  let hash = Math.imul(x, 73_856_093) ^ Math.imul(y, 19_349_663) ^ Math.imul(z, 83_492_791);
  hash = Math.imul(hash ^ (hash >>> 16), 0x45d9f3b);
  return (hash ^ (hash >>> 16)) & (COARSE_VOTE_CAPACITY - 1);
}

function addTranslationVote(
  buffer: TranslationVoteBuffer,
  tx: number,
  ty: number,
  tz: number,
): boolean {
  const x = Math.floor(tx / COARSE_TRANSLATION_CELL_METERS);
  const y = Math.floor(ty / COARSE_TRANSLATION_CELL_METERS);
  const z = Math.floor(tz / COARSE_TRANSLATION_Z_CELL_METERS);
  if (![x, y, z].every((value) => Number.isSafeInteger(value) && value >= -0x80000000 && value <= 0x7fffffff)) return false;
  let slot = translationCellHash(x, y, z);
  while (true) {
    if (buffer.count[slot] === 0) {
      buffer.x[slot] = x;
      buffer.y[slot] = y;
      buffer.z[slot] = z;
      buffer.count[slot] = 1;
      buffer.sumX[slot] = tx;
      buffer.sumY[slot] = ty;
      buffer.sumZ[slot] = tz;
      buffer.usedSlots[buffer.usedCount++] = slot;
      return true;
    }
    if (buffer.x[slot] === x && buffer.y[slot] === y && buffer.z[slot] === z) {
      buffer.count[slot] += 1;
      buffer.sumX[slot] += tx;
      buffer.sumY[slot] += ty;
      buffer.sumZ[slot] += tz;
      return true;
    }
    slot = (slot + 1) & (COARSE_VOTE_CAPACITY - 1);
  }
}

function isVoteBinBefore(a: number, b: number, buffer: TranslationVoteBuffer): boolean {
  if (buffer.count[a] !== buffer.count[b]) return buffer.count[a] > buffer.count[b];
  return buffer.x[a] < buffer.x[b] ||
    (buffer.x[a] === buffer.x[b] && (buffer.y[a] < buffer.y[b] ||
      (buffer.y[a] === buffer.y[b] && buffer.z[a] < buffer.z[b])));
}

function topTranslationVoteBins(buffer: TranslationVoteBuffer): number[] {
  const top: number[] = [];
  for (let used = 0; used < buffer.usedCount; used += 1) {
    const candidate = buffer.usedSlots[used];
    if (buffer.count[candidate] < COARSE_MIN_VOTES) continue;
    let index = 0;
    while (index < top.length && !isVoteBinBefore(candidate, top[index], buffer)) index += 1;
    if (index >= COARSE_TOP_BINS_PER_YAW) continue;
    top.splice(index, 0, candidate);
    if (top.length > COARSE_TOP_BINS_PER_YAW) top.pop();
  }
  return top;
}

function clearTranslationVoteBins(buffer: TranslationVoteBuffer): void {
  for (let index = 0; index < buffer.usedCount; index += 1) buffer.count[buffer.usedSlots[index]] = 0;
  buffer.usedCount = 0;
}

function compareRegistrationCandidates(a: RegistrationCandidate, b: RegistrationCandidate): number {
  return b.matches.length - a.matches.length || a.rmsMeters - b.rmsMeters;
}

function retainRegistrationCandidate(
  candidates: RegistrationCandidate[],
  candidate: RegistrationCandidate,
): void {
  const duplicateIndex = candidates.findIndex((previous) => {
    const translationSeparation = Math.hypot(
      candidate.transform.tx - previous.transform.tx,
      candidate.transform.ty - previous.transform.ty,
      candidate.transform.tz - previous.transform.tz,
    );
    return translationSeparation <= 0.25 &&
      yawSeparation(candidate.transform.yaw, previous.transform.yaw) <= 5 * Math.PI / 180;
  });
  if (duplicateIndex >= 0) {
    if (compareRegistrationCandidates(candidate, candidates[duplicateIndex]) < 0) candidates[duplicateIndex] = candidate;
  } else {
    candidates.push(candidate);
  }
  candidates.sort(compareRegistrationCandidates);
  if (candidates.length > MAX_RETAINED_REGISTRATION_CANDIDATES) candidates.length = MAX_RETAINED_REGISTRATION_CANDIDATES;
}

function localDistanceDescriptors(points: readonly Point3[], operations: RegistrationWorkCounters): Float32Array {
  const descriptors = new Float32Array(points.length * FEATURE_NEIGHBOR_COUNT);
  for (let index = 0; index < points.length; index += 1) {
    const nearest = [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY];
    const point = points[index];
    for (let otherIndex = 0; otherIndex < points.length; otherIndex += 1) {
      if (otherIndex === index) continue;
      operations.localDescriptorDistanceChecks += 1;
      const other = points[otherIndex];
      const dx = other.x - point.x, dy = other.y - point.y, dz = other.z - point.z;
      const distance = Math.sqrt(dx * dx + dy * dy + dz * dz);
      for (let neighbor = 0; neighbor < nearest.length; neighbor += 1) {
        if (distance >= nearest[neighbor]) continue;
        for (let move = nearest.length - 1; move > neighbor; move -= 1) nearest[move] = nearest[move - 1];
        nearest[neighbor] = distance;
        break;
      }
    }
    const offset = index * FEATURE_NEIGHBOR_COUNT;
    for (let neighbor = 0; neighbor < FEATURE_NEIGHBOR_COUNT; neighbor += 1) descriptors[offset + neighbor] = nearest[neighbor];
  }
  return descriptors;
}

interface RegistrationFeatureData {
  baseDescriptors: Float32Array;
  extensionDescriptors: Float32Array;
  pairs: Array<{ source: Point3; target: Point3 }>;
}

function featureDescriptorError(
  source: Point3,
  target: Point3,
  features: RegistrationFeatureData,
): number {
  const sourceOffset = source.featureIndex * FEATURE_NEIGHBOR_COUNT;
  const targetOffset = target.featureIndex * FEATURE_NEIGHBOR_COUNT;
  let error = 0;
  for (let neighbor = 0; neighbor < FEATURE_NEIGHBOR_COUNT; neighbor += 1) {
    error += Math.abs(
      features.extensionDescriptors[sourceOffset + neighbor] - features.baseDescriptors[targetOffset + neighbor],
    );
  }
  return error;
}

function featureCorrespondences(
  basePoints: readonly Point3[],
  extensionPoints: readonly Point3[],
  operations: RegistrationWorkCounters,
): RegistrationFeatureData {
  const baseDescriptors = localDistanceDescriptors(basePoints, operations);
  const extensionDescriptors = localDistanceDescriptors(extensionPoints, operations);
  const result: Array<{ source: Point3; target: Point3 }> = [];
  const bestIndices = new Int32Array(FEATURE_MATCHES_PER_SOURCE);
  const bestErrors = new Float64Array(FEATURE_MATCHES_PER_SOURCE);
  for (let sourceIndex = 0; sourceIndex < extensionPoints.length; sourceIndex += 1) {
    bestIndices.fill(-1);
    bestErrors.fill(Number.POSITIVE_INFINITY);
    const sourceOffset = sourceIndex * FEATURE_NEIGHBOR_COUNT;
    for (let targetIndex = 0; targetIndex < basePoints.length; targetIndex += 1) {
      operations.featureDescriptorComparisons += 1;
      const targetOffset = targetIndex * FEATURE_NEIGHBOR_COUNT;
      let error = 0;
      for (let neighbor = 0; neighbor < FEATURE_NEIGHBOR_COUNT; neighbor += 1) {
        error += Math.abs(extensionDescriptors[sourceOffset + neighbor] - baseDescriptors[targetOffset + neighbor]);
      }
      if (error >= bestErrors[FEATURE_MATCHES_PER_SOURCE - 1]) continue;
      let position = FEATURE_MATCHES_PER_SOURCE - 1;
      while (position > 0 && error < bestErrors[position - 1]) {
        bestErrors[position] = bestErrors[position - 1];
        bestIndices[position] = bestIndices[position - 1];
        position -= 1;
      }
      bestErrors[position] = error;
      bestIndices[position] = targetIndex;
    }
    for (let candidate = 0; candidate < FEATURE_MATCHES_PER_SOURCE; candidate += 1) {
      const targetIndex = bestIndices[candidate];
      if (targetIndex < 0 || bestErrors[candidate] > FEATURE_DESCRIPTOR_MAX_ERROR) continue;
      result.push({ source: extensionPoints[sourceIndex], target: basePoints[targetIndex] });
      operations.featureCandidatePairs += 1;
    }
  }
  return { baseDescriptors, extensionDescriptors, pairs: result };
}

function horizontalConditioning(matches: readonly Match[]): {
  horizontalBaselineMeters: number;
  horizontalMinorVarianceMeters2: number;
  horizontalConditionRatio: number;
} {
  if (matches.length === 0) {
    return { horizontalBaselineMeters: 0, horizontalMinorVarianceMeters2: 0, horizontalConditionRatio: 0 };
  }
  let meanX = 0, meanY = 0;
  for (const match of matches) {
    meanX += match.source.x;
    meanY += match.source.y;
  }
  meanX /= matches.length;
  meanY /= matches.length;
  let xx = 0, xy = 0, yy = 0;
  for (const match of matches) {
    const dx = match.source.x - meanX;
    const dy = match.source.y - meanY;
    xx += dx * dx;
    xy += dx * dy;
    yy += dy * dy;
  }
  xx /= matches.length;
  xy /= matches.length;
  yy /= matches.length;
  const trace = xx + yy;
  const root = Math.sqrt(Math.max(0, (xx - yy) ** 2 + 4 * xy * xy));
  const maximumVariance = Math.max(0, (trace + root) / 2);
  const minimumVariance = Math.max(0, (trace - root) / 2);
  return {
    horizontalBaselineMeters: 2 * Math.sqrt(maximumVariance),
    horizontalMinorVarianceMeters2: minimumVariance,
    horizontalConditionRatio: maximumVariance > 0 ? minimumVariance / maximumVariance : 0,
  };
}

function yawSeparation(a: number, b: number): number {
  const difference = Math.abs(a - b) % (Math.PI * 2);
  return Math.min(difference, Math.PI * 2 - difference);
}

function mergeSimplePoints(
  basePositions: Float32Array,
  baseConfidence: Float32Array,
  extensionPositions: Float32Array,
  extensionConfidence: Float32Array,
  stablePositions: Float32Array,
  voxelSizeMeters: number,
): { positions: Float32Array; confidence: Float32Array } {
  const blocked = new Set<string>();
  for (let index = 0; index < pointCount(stablePositions); index += 1) {
    const offset = index * 3;
    blocked.add(voxelKey(stablePositions[offset], stablePositions[offset + 1], stablePositions[offset + 2], voxelSizeMeters));
  }
  const keys = new Set<string>();
  const positions: number[] = [];
  const confidence: number[] = [];
  const add = (p: Float32Array, c: Float32Array, index: number): void => {
    const offset = index * 3;
    const key = voxelKey(p[offset], p[offset + 1], p[offset + 2], voxelSizeMeters);
    if (blocked.has(key) || keys.has(key)) return;
    keys.add(key);
    positions.push(p[offset], p[offset + 1], p[offset + 2]);
    confidence.push(c[index] ?? 0);
  };
  for (let index = 0; index < pointCount(basePositions); index += 1) add(basePositions, baseConfidence, index);
  for (let index = 0; index < pointCount(extensionPositions); index += 1) add(extensionPositions, extensionConfidence, index);
  return { positions: new Float32Array(positions), confidence: new Float32Array(confidence) };
}

function assertCompleteOccupancy(snapshot: ScanPointCloudSnapshot): string | null {
  if (pointCount(snapshot.freeAirPositions) !== snapshot.knownFreeVoxelCount) return 'saved FREE evidence is display-truncated';
  if (pointCount(snapshot.occupiedSurfacePositions) !== snapshot.occupiedSurfaceVoxelCount) return 'saved OCCUPIED evidence is display-truncated';
  if (pointCount(snapshot.unknownCandidatePositions) !== snapshot.unknownCandidateVoxelCount) return 'saved UNKNOWN evidence is display-truncated';
  return null;
}

function positionsMap(values: Float32Array, voxelSize: number): Map<string, readonly [number, number, number]> {
  const result = new Map<string, readonly [number, number, number]>();
  for (let index = 0; index < pointCount(values); index += 1) {
    const offset = index * 3;
    const point: readonly [number, number, number] = [values[offset], values[offset + 1], values[offset + 2]];
    result.set(voxelKey(point[0], point[1], point[2], voxelSize), point);
  }
  return result;
}

function mapToPositions(map: ReadonlyMap<string, readonly [number, number, number]>): Float32Array {
  const result = new Float32Array(map.size * 3);
  let offset = 0;
  for (const point of map.values()) {
    result[offset++] = point[0]; result[offset++] = point[1]; result[offset++] = point[2];
  }
  return result;
}

function mergeOccupancy(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  transform: RigidTransform,
  envelopeBounds: ScanPointCloudSnapshot['occupancyEnvelopeBounds'],
): {
  freeAirPositions: Float32Array;
  occupiedSurfacePositions: Float32Array;
  unknownCandidatePositions: Float32Array;
  occupancyVoxelSizeMeters: number;
} | null {
  if (assertCompleteOccupancy(base) !== null || assertCompleteOccupancy(extension) !== null) return null;
  const voxel = fineOccupancyVoxelSize(base);
  if (Math.abs(voxel - fineOccupancyVoxelSize(extension)) > 1e-6) return null;
  const occupied = positionsMap(base.occupiedSurfacePositions, voxel);
  for (const [key, point] of positionsMap(transformPositions(extension.occupiedSurfacePositions, transform), voxel)) occupied.set(key, point);
  const free = positionsMap(base.freeAirPositions, voxel);
  for (const [key, point] of positionsMap(transformPositions(extension.freeAirPositions, transform), voxel)) {
    if (!occupied.has(key)) free.set(key, point);
  }
  for (const key of occupied.keys()) free.delete(key);
  const unknown = enumerateUnknownCandidatePositions(
    envelopeBounds as OccupancyBounds | null,
    voxel,
    (x, y, z) => !occupied.has(`${x}:${y}:${z}`) && !free.has(`${x}:${y}:${z}`),
  );
  return {
    freeAirPositions: mapToPositions(free),
    occupiedSurfacePositions: mapToPositions(occupied),
    unknownCandidatePositions: unknown.unknownCandidatePositions,
    occupancyVoxelSizeMeters: unknown.occupancyVoxelSizeMeters,
  };
}

function mergeTsdf(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  transform: RigidTransform,
): Pick<ScanPointCloudSnapshot,
  'tsdfPositions' | 'tsdfNormals' | 'tsdfConfidence' | 'tsdfResidualMeters' |
  'tsdfNormalConfidence' | 'tsdfCoverageConfidence' | 'tsdfCoverageReady' | 'tsdfPointCount'> {
  /* Select one atomic sample per voxel: known normal, coverage, confidence, residual, then base. */
  const extensionPositions = transformPositions(extension.tsdfPositions, transform);
  const extensionNormals = transformNormals(extension.tsdfNormals, transform);
  const voxel = base.voxelSizeMeters;
  interface TsdfEvidence {
    key: string;
    x: number; y: number; z: number;
    nx: number; ny: number; nz: number;
    confidence: number;
    residual: number;
    normalConfidence: number;
    coverageConfidence: number;
    coverageReady: number;
    sourceRank: 0 | 1;
  }
  const selected = new Map<string, TsdfEvidence>();
  const isBetter = (candidate: TsdfEvidence, previous: TsdfEvidence): boolean => {
    const candidateKnownNormal = candidate.normalConfidence > 0;
    const previousKnownNormal = previous.normalConfidence > 0;
    if (candidateKnownNormal !== previousKnownNormal) return candidateKnownNormal;
    const candidateReady = candidate.coverageReady !== 0;
    const previousReady = previous.coverageReady !== 0;
    if (candidateReady !== previousReady) return candidateReady;
    if (candidate.confidence !== previous.confidence) return candidate.confidence > previous.confidence;
    if (candidate.residual !== previous.residual) return candidate.residual < previous.residual;
    if (candidate.sourceRank !== previous.sourceRank) return candidate.sourceRank < previous.sourceRank;
    const candidateValues = [
      candidate.x, candidate.y, candidate.z, candidate.nx, candidate.ny, candidate.nz,
      candidate.normalConfidence, candidate.coverageConfidence,
    ];
    const previousValues = [
      previous.x, previous.y, previous.z, previous.nx, previous.ny, previous.nz,
      previous.normalConfidence, previous.coverageConfidence,
    ];
    for (let index = 0; index < candidateValues.length; index += 1) {
      if (candidateValues[index] !== previousValues[index]) return candidateValues[index] < previousValues[index];
    }
    return false;
  };
  const add = (
    source: ScanPointCloudSnapshot,
    p: Float32Array,
    n: Float32Array,
    index: number,
    sourceRank: 0 | 1,
  ): void => {
    const offset = index * 3;
    const key = voxelKey(p[offset], p[offset + 1], p[offset + 2], voxel);
    const candidate: TsdfEvidence = {
      key,
      x: p[offset], y: p[offset + 1], z: p[offset + 2],
      nx: n[offset], ny: n[offset + 1], nz: n[offset + 2],
      confidence: source.tsdfConfidence[index] ?? 0,
      residual: source.tsdfResidualMeters[index] ?? 0,
      normalConfidence: source.tsdfNormalConfidence[index] ?? 0,
      coverageConfidence: source.tsdfCoverageConfidence[index] ?? 0,
      coverageReady: source.tsdfCoverageReady[index] ?? 0,
      sourceRank,
    };
    const previous = selected.get(key);
    if (previous === undefined || isBetter(candidate, previous)) selected.set(key, candidate);
  };
  for (let index = 0; index < base.tsdfPointCount; index += 1) {
    add(base, base.tsdfPositions, base.tsdfNormals, index, 0);
  }
  for (let index = 0; index < extension.tsdfPointCount; index += 1) {
    add(extension, extensionPositions, extensionNormals, index, 1);
  }
  const ordered = [...selected.values()].sort((a, b) => a.key < b.key ? -1 : a.key > b.key ? 1 : 0);
  const positions = new Float32Array(ordered.length * 3);
  const normals = new Float32Array(ordered.length * 3);
  const confidence = new Float32Array(ordered.length);
  const residual = new Float32Array(ordered.length);
  const normalConfidence = new Float32Array(ordered.length);
  const coverageConfidence = new Float32Array(ordered.length);
  const coverageReady = new Uint8Array(ordered.length);
  for (let index = 0; index < ordered.length; index += 1) {
    const evidence = ordered[index];
    const offset = index * 3;
    positions[offset] = evidence.x; positions[offset + 1] = evidence.y; positions[offset + 2] = evidence.z;
    normals[offset] = evidence.nx; normals[offset + 1] = evidence.ny; normals[offset + 2] = evidence.nz;
    confidence[index] = evidence.confidence;
    residual[index] = evidence.residual;
    normalConfidence[index] = evidence.normalConfidence;
    coverageConfidence[index] = evidence.coverageConfidence;
    coverageReady[index] = evidence.coverageReady;
  }
  return {
    tsdfPositions: positions,
    tsdfNormals: normals,
    tsdfConfidence: confidence,
    tsdfResidualMeters: residual,
    tsdfNormalConfidence: normalConfidence,
    tsdfCoverageConfidence: coverageConfidence,
    tsdfCoverageReady: coverageReady,
    tsdfPointCount: confidence.length,
  };
}

function boundsFor(values: Float32Array): ScanPointCloudSnapshot['bounds'] {
  if (values.length < 3) return null;
  let minX = Number.POSITIVE_INFINITY, minY = Number.POSITIVE_INFINITY, minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY, maxY = Number.NEGATIVE_INFINITY, maxZ = Number.NEGATIVE_INFINITY;
  for (let offset = 0; offset + 2 < values.length; offset += 3) {
    minX = Math.min(minX, values[offset]); minY = Math.min(minY, values[offset + 1]); minZ = Math.min(minZ, values[offset + 2]);
    maxX = Math.max(maxX, values[offset]); maxY = Math.max(maxY, values[offset + 1]); maxZ = Math.max(maxZ, values[offset + 2]);
  }
  return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

function transformBounds(
  bounds: ScanPointCloudSnapshot['occupancyEnvelopeBounds'],
  transform: RigidTransform,
): ScanPointCloudSnapshot['occupancyEnvelopeBounds'] {
  if (bounds === null) return null;
  let minX = Number.POSITIVE_INFINITY, minY = Number.POSITIVE_INFINITY, minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY, maxY = Number.NEGATIVE_INFINITY, maxZ = Number.NEGATIVE_INFINITY;
  for (let corner = 0; corner < 8; corner += 1) {
    const point = transformPoint({
      x: (corner & 1) === 0 ? bounds.min[0] : bounds.max[0],
      y: (corner & 2) === 0 ? bounds.min[1] : bounds.max[1],
      z: (corner & 4) === 0 ? bounds.min[2] : bounds.max[2],
      sourceIndex: corner,
      featureIndex: corner,
    }, transform);
    minX = Math.min(minX, point.x); minY = Math.min(minY, point.y); minZ = Math.min(minZ, point.z);
    maxX = Math.max(maxX, point.x); maxY = Math.max(maxY, point.y); maxZ = Math.max(maxZ, point.z);
  }
  return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

function mergeOccupancyEnvelopeBounds(
  base: ScanPointCloudSnapshot['occupancyEnvelopeBounds'],
  extension: ScanPointCloudSnapshot['occupancyEnvelopeBounds'],
  transform: RigidTransform,
): ScanPointCloudSnapshot['occupancyEnvelopeBounds'] {
  const transformedExtension = transformBounds(extension, transform);
  if (base === null) return transformedExtension;
  if (transformedExtension === null) return base;
  return {
    min: [
      Math.min(base.min[0], transformedExtension.min[0]),
      Math.min(base.min[1], transformedExtension.min[1]),
      Math.min(base.min[2], transformedExtension.min[2]),
    ],
    max: [
      Math.max(base.max[0], transformedExtension.max[0]),
      Math.max(base.max[1], transformedExtension.max[1]),
      Math.max(base.max[2], transformedExtension.max[2]),
    ],
  };
}

function transformedManualPoints(snapshot: ScanPointCloudSnapshot, transform: RigidTransform, startId: number) {
  return snapshot.manualControlPoints.map((point, index) => {
    const transformed = transformPoint({ x: point.position[0], y: point.position[1], z: point.position[2], sourceIndex: 0, featureIndex: 0 }, transform);
    return { id: startId + index, position: [transformed.x, transformed.y, transformed.z] as const };
  });
}

function transformedEdges(snapshot: ScanPointCloudSnapshot, transform: RigidTransform, startId: number) {
  return snapshot.operatorEdges.map((edge, index) => {
    const a = transformPoint({ x: edge.start[0], y: edge.start[1], z: edge.start[2], sourceIndex: 0, featureIndex: 0 }, transform);
    const b = transformPoint({ x: edge.end[0], y: edge.end[1], z: edge.end[2], sourceIndex: 0, featureIndex: 0 }, transform);
    return {
      id: startId + index,
      start: [a.x, a.y, a.z] as const,
      end: [b.x, b.y, b.z] as const,
      confidence: edge.confidence,
    };
  });
}

function mergeAnnotations(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  transform: RigidTransform,
): {
  manualControlPoints: ScanPointCloudSnapshot['manualControlPoints'];
  operatorEdges: ScanPointCloudSnapshot['operatorEdges'];
  annotationsTruncated: boolean;
} {
  const manualControlPoints: Array<ScanPointCloudSnapshot['manualControlPoints'][number]> = [];
  const operatorEdges: Array<ScanPointCloudSnapshot['operatorEdges'][number]> = [];
  let annotationsTruncated = false;
  const addManual = (point: ScanPointCloudSnapshot['manualControlPoints'][number]): void => {
    if (manualControlPoints.length >= MAX_MANUAL_CONTROL_POINTS ||
        serializedAnnotationBytes([...manualControlPoints, point], operatorEdges) > MAX_ANNOTATION_JSON_BYTES) {
      annotationsTruncated = true;
      return;
    }
    manualControlPoints.push(point);
  };
  const addEdge = (edge: ScanPointCloudSnapshot['operatorEdges'][number]): void => {
    if (operatorEdges.length >= MAX_OPERATOR_EDGES ||
        serializedAnnotationBytes(manualControlPoints, [...operatorEdges, edge]) > MAX_ANNOTATION_JSON_BYTES) {
      annotationsTruncated = true;
      return;
    }
    operatorEdges.push(edge);
  };

  /* Preserve all accepted base annotations first; extension entries follow by stable ID order. */
  for (const point of [...base.manualControlPoints].sort((a, b) => a.id - b.id)) addManual(point);
  for (const edge of [...base.operatorEdges].sort((a, b) => a.id - b.id)) addEdge(edge);
  const maxManualId = base.manualControlPoints.reduce((max, point) => Math.max(max, point.id), 0);
  const maxEdgeId = base.operatorEdges.reduce((max, edge) => Math.max(max, edge.id), 0);
  const orderedExtension: ScanPointCloudSnapshot = {
    ...extension,
    manualControlPoints: [...extension.manualControlPoints].sort((a, b) => a.id - b.id),
    operatorEdges: [...extension.operatorEdges].sort((a, b) => a.id - b.id),
  };
  for (const point of transformedManualPoints(orderedExtension, transform, maxManualId + 1)) addManual(point);
  for (const edge of transformedEdges(orderedExtension, transform, maxEdgeId + 1)) addEdge(edge);
  return { manualControlPoints, operatorEdges, annotationsTruncated };
}

function weightedAverage(a: number, aCount: number, b: number, bCount: number): number {
  const total = aCount + bCount;
  return total > 0 ? (a * aCount + b * bCount) / total : 0;
}

function unknownCandidateCellUpperBound(
  bounds: ScanPointCloudSnapshot['occupancyEnvelopeBounds'],
  fineVoxelSizeMeters: number,
): number {
  if (bounds === null) return 0;
  const minX = Math.floor(bounds.min[0] / fineVoxelSizeMeters);
  const minY = Math.floor(bounds.min[1] / fineVoxelSizeMeters);
  const minZ = Math.floor(bounds.min[2] / fineVoxelSizeMeters);
  const maxX = Math.floor(bounds.max[0] / fineVoxelSizeMeters);
  const maxY = Math.floor(bounds.max[1] / fineVoxelSizeMeters);
  const maxZ = Math.floor(bounds.max[2] / fineVoxelSizeMeters);
  const nx = Math.max(1, maxX - minX + 1);
  const ny = Math.max(1, maxY - minY + 1);
  const nz = Math.max(1, maxZ - minZ + 1);
  const volume = nx * ny * nz;
  if (!Number.isFinite(volume)) return Number.MAX_SAFE_INTEGER;
  const stride = Math.max(1, Math.ceil(Math.cbrt(volume / UNKNOWN_ENUMERATION_MAX_CELLS)));
  const coarseX = Math.floor(maxX / stride) - Math.floor(minX / stride) + 1;
  const coarseY = Math.floor(maxY / stride) - Math.floor(minY / stride) + 1;
  const coarseZ = Math.floor(maxZ / stride) - Math.floor(minZ / stride) + 1;
  const upperBound = coarseX * coarseY * coarseZ;
  return Number.isFinite(upperBound) ? upperBound : Number.MAX_SAFE_INTEGER;
}

function estimateMergedArtifactBudget(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  envelopeBounds: ScanPointCloudSnapshot['occupancyEnvelopeBounds'],
  annotationJsonBytes: number,
): {
  estimatedBytes: number;
  limitBytes: number;
  arrayBytes: number;
  headerReserveBytes: number;
  families: Readonly<Record<string, number>>;
} {
  const stablePoints = pointCount(base.positions) + pointCount(extension.positions);
  const tentativePoints = pointCount(base.tentativePositions) + pointCount(extension.tentativePositions);
  const freePoints = pointCount(base.freeAirPositions) + pointCount(extension.freeAirPositions);
  const occupiedPoints = pointCount(base.occupiedSurfacePositions) + pointCount(extension.occupiedSurfacePositions);
  const unknownPoints = unknownCandidateCellUpperBound(envelopeBounds, fineOccupancyVoxelSize(base));
  const tsdfPoints =
    Math.max(base.tsdfPointCount, pointCount(base.tsdfPositions)) +
    Math.max(extension.tsdfPointCount, pointCount(extension.tsdfPositions));
  const families: Record<string, number> = {
    stablePositions: stablePoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    stableConfidence: stablePoints * Float32Array.BYTES_PER_ELEMENT,
    stableNormals: stablePoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    stableNormalConfidence: base.normalConfidence === undefined && extension.normalConfidence === undefined
      ? 0
      : stablePoints * Float32Array.BYTES_PER_ELEMENT,
    tentativePositions: tentativePoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    tentativeConfidence: tentativePoints * Float32Array.BYTES_PER_ELEMENT,
    freeAirPositions: freePoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    occupiedSurfacePositions: occupiedPoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    unknownCandidatePositions: unknownPoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    tsdfPositions: tsdfPoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    tsdfNormals: tsdfPoints * 3 * Float32Array.BYTES_PER_ELEMENT,
    tsdfConfidence: tsdfPoints * Float32Array.BYTES_PER_ELEMENT,
    tsdfResidualMeters: tsdfPoints * Float32Array.BYTES_PER_ELEMENT,
    tsdfNormalConfidence: tsdfPoints * Float32Array.BYTES_PER_ELEMENT,
    tsdfCoverageConfidence: tsdfPoints * Float32Array.BYTES_PER_ELEMENT,
    tsdfCoverageReady: tsdfPoints * Uint8Array.BYTES_PER_ELEMENT,
    annotationJson: annotationJsonBytes,
  };
  const arrayBytes = Object.entries(families)
    .filter(([key]) => key !== 'annotationJson')
    .reduce((sum, [, bytes]) => sum + bytes, 0);
  const headerReserveBytes = SCAN_ARTIFACT_FIXED_HEADER_BYTES + annotationJsonBytes + SCAN_ARTIFACT_METADATA_RESERVE_BYTES;
  return {
    estimatedBytes: arrayBytes + headerReserveBytes,
    limitBytes: MAX_SCAN_ARTIFACT_BYTES,
    arrayBytes,
    headerReserveBytes,
    families,
  };
}

export function registerAndMergeScanContinuation(
  base: ScanPointCloudSnapshot,
  extension: ScanPointCloudSnapshot,
  hooks?: ScanContinuationHooks,
): ScanContinuationResult {
  if (base.pointCount < 120 || extension.pointCount < 120) {
    return { ok: false, reason: 'Need at least 120 stable points in both the saved scan and the continuation.', diagnostics: null };
  }
  const occupancyProblem = assertCompleteOccupancy(base) ?? assertCompleteOccupancy(extension);
  if (occupancyProblem !== null) {
    return { ok: false, reason: `Cannot safely continue: ${occupancyProblem}. Old saved scan was kept.`, diagnostics: null };
  }
  if (Math.abs(base.voxelSizeMeters - extension.voxelSizeMeters) > 1e-6) {
    return {
      ok: false,
      reason: 'Cannot safely continue scans with different surface voxel sizes; resampling would change saved evidence. Old saved scan was kept.',
      diagnostics: null,
    };
  }
  if (Math.abs(fineOccupancyVoxelSize(base) - fineOccupancyVoxelSize(extension)) > 1e-6) {
    return { ok: false, reason: 'Cannot safely continue scans with different occupancy fine voxel sizes.', diagnostics: null };
  }

  reportContinuationPhase(hooks, 'registration');
  const registration = registerExtension(base, extension);
  if (registration === null) {
    return { ok: false, reason: 'Could not align the continuation. Start by scanning an overlapping part of the saved scene.', diagnostics: null };
  }
  const diagnostics = registration.diagnostics;
  if (diagnostics.rejectionReason === 'ambiguous_overlap') {
    return {
      ok: false,
      reason: 'Continuation alignment is ambiguous. Scan more overlap and add distinctive geometry before continuing; old saved scan was kept.',
      diagnostics,
    };
  }
  if (diagnostics.rejectionReason === 'insufficient_conditioning') {
    return {
      ok: false,
      reason: 'Continuation overlap has too little horizontal spread to establish a stable alignment. Scan more overlap with distinctive geometry; old saved scan was kept.',
      diagnostics,
    };
  }
  if (
    diagnostics.inliers < REGISTRATION_MIN_INLIERS ||
    diagnostics.overlapRatio < REGISTRATION_MIN_OVERLAP ||
    diagnostics.rmsMeters > REGISTRATION_MAX_RMS_METERS
  ) {
    return {
      ok: false,
      reason: `Continuation alignment rejected: ${diagnostics.inliers} matches, ${Math.round(diagnostics.overlapRatio * 100)}% overlap, ${(diagnostics.rmsMeters * 100).toFixed(1)} cm RMS. Scan more overlap; old saved scan was kept.`,
      diagnostics,
    };
  }

  const transform = registration.transform;
  const occupancyEnvelopeBounds = mergeOccupancyEnvelopeBounds(
    base.occupancyEnvelopeBounds,
    extension.occupancyEnvelopeBounds,
    transform,
  );
  const annotations = mergeAnnotations(base, extension, transform);
  const budgetEstimate = estimateMergedArtifactBudget(
    base,
    extension,
    occupancyEnvelopeBounds,
    serializedAnnotationBytes(annotations.manualControlPoints, annotations.operatorEdges),
  );
  diagnostics.mergedBudgetEstimate = budgetEstimate;
  if (budgetEstimate.estimatedBytes > budgetEstimate.limitBytes) {
    diagnostics.rejectionReason = 'merged_budget_exceeded';
    diagnostics.annotationsTruncated = annotations.annotationsTruncated;
    return {
      ok: false,
      reason: `Cannot safely continue: conservative merged artifact estimate ${budgetEstimate.estimatedBytes} bytes exceeds the ${budgetEstimate.limitBytes}-byte limit (arrays ${budgetEstimate.arrayBytes} bytes; header reserve ${budgetEstimate.headerReserveBytes} bytes). Old saved scan was kept.`,
      diagnostics,
    };
  }

  reportContinuationPhase(hooks, 'stable');
  const extensionStablePositions = transformPositions(extension.positions, transform);
  const extensionStableNormals = transformNormals(extension.normals, transform);
  const mergeVoxel = base.voxelSizeMeters;
  const stable = mergeSurface(
    base.positions,
    base.confidence,
    base.normals,
    base.normalConfidence,
    extensionStablePositions,
    extension.confidence,
    extensionStableNormals,
    extension.normalConfidence,
    mergeVoxel,
  );
  const tentative = mergeSimplePoints(
    base.tentativePositions,
    base.tentativeConfidence,
    transformPositions(extension.tentativePositions, transform),
    extension.tentativeConfidence,
    stable.positions,
    mergeVoxel,
  );
  reportContinuationPhase(hooks, 'occupancy');
  const occupancy = mergeOccupancy(base, extension, transform, occupancyEnvelopeBounds);
  if (occupancy === null) {
    return { ok: false, reason: 'Continuation occupancy evidence could not be merged safely. Old saved scan was kept.', diagnostics };
  }
  reportContinuationPhase(hooks, 'tsdf');
  const tsdf = mergeTsdf(base, extension, transform);
  const fullPositions = appendFloatArrays(stable.positions, tentative.positions);
  const mergedPointCount = pointCount(stable.positions) + pointCount(tentative.positions);

  const snapshot: ScanPointCloudSnapshot = {
    ...base,
    positions: stable.positions,
    confidence: stable.confidence,
    normals: stable.normals,
    ...(stable.normalConfidence === undefined ? {} : { normalConfidence: stable.normalConfidence }),
    pointCount: pointCount(stable.positions),
    tentativePositions: tentative.positions,
    tentativeConfidence: tentative.confidence,
    tentativePointCount: pointCount(tentative.positions),
    rawPointCount: base.rawPointCount + extension.rawPointCount,
    bounds: boundsFor(stable.positions),
    fullBounds: boundsFor(fullPositions),
    maxDepthMeters: Math.max(base.maxDepthMeters, extension.maxDepthMeters),
    maxPointCapacity: base.maxPointCapacity + extension.maxPointCapacity,
    memoryBudgetPoints: base.memoryBudgetPoints + extension.memoryBudgetPoints,
    coverageCellSizeMeters: Math.min(base.coverageCellSizeMeters, extension.coverageCellSizeMeters),
    adaptivePointsPerCell: Math.max(base.adaptivePointsPerCell, extension.adaptivePointsPerCell),
    boundsTrimFraction: weightedAverage(
      base.boundsTrimFraction,
      base.pointCount,
      extension.boundsTrimFraction,
      extension.pointCount,
    ),
    depthFrameCount: base.depthFrameCount + extension.depthFrameCount,
    sampledDepthFrames: base.sampledDepthFrames + extension.sampledDepthFrames,
    capacityReached: base.capacityReached || extension.capacityReached,
    candidateSampleCount: base.candidateSampleCount + extension.candidateSampleCount,
    discardedSampleCount: base.discardedSampleCount + extension.discardedSampleCount,
    replacementCount: base.replacementCount + extension.replacementCount,
    rebalanceEvictionCount: base.rebalanceEvictionCount + extension.rebalanceEvictionCount,
    coverageRejectedCount: base.coverageRejectedCount + extension.coverageRejectedCount,
    promotedPointCount: pointCount(stable.positions),
    integrationKeyframes: base.integrationKeyframes + extension.integrationKeyframes,
    polishFrames: base.polishFrames + extension.polishFrames,
    residualRejectedSamples: base.residualRejectedSamples + extension.residualRejectedSamples,
    staleTentativePruned: base.staleTentativePruned + extension.staleTentativePruned,
    averageStableResidualMeters: weightedAverage(
      base.averageStableResidualMeters,
      base.pointCount,
      extension.averageStableResidualMeters,
      extension.pointCount,
    ),
    coverageCellCount: base.coverageCellCount + extension.coverageCellCount,
    readyCoverageCellCount: base.readyCoverageCellCount + extension.readyCoverageCellCount,
    freeAirPositions: occupancy.freeAirPositions,
    occupiedSurfacePositions: occupancy.occupiedSurfacePositions,
    unknownCandidatePositions: occupancy.unknownCandidatePositions,
    occupancyVoxelSizeMeters: occupancy.occupancyVoxelSizeMeters,
    knownFreeVoxelCount: pointCount(occupancy.freeAirPositions),
    occupiedSurfaceVoxelCount: pointCount(occupancy.occupiedSurfacePositions),
    unknownCandidateVoxelCount: pointCount(occupancy.unknownCandidatePositions),
    occupancyEnvelopeBounds,
    occupancySolverCandidateBelowSurfaceMeters: occupancyEnvelopeBounds === null
      ? 0
      : Math.max(base.occupancySolverCandidateBelowSurfaceMeters ?? 0, extension.occupancySolverCandidateBelowSurfaceMeters ?? 0),
    // Capture counters aggregate both acquisitions; they are provenance counts, not deduplicated cells.
    occupancyEnvelopeComponentCount: base.occupancyEnvelopeComponentCount + extension.occupancyEnvelopeComponentCount,
    occupancyQuarantinedSurfaceComponentCount: base.occupancyQuarantinedSurfaceComponentCount + extension.occupancyQuarantinedSurfaceComponentCount,
    occupancyQuarantinedSurfacePointCount: base.occupancyQuarantinedSurfacePointCount + extension.occupancyQuarantinedSurfacePointCount,
    occupancyBridgeRescanComponentCount: base.occupancyBridgeRescanComponentCount + extension.occupancyBridgeRescanComponentCount,
    occupancyBridgeEvidenceTruncated: base.occupancyBridgeEvidenceTruncated || extension.occupancyBridgeEvidenceTruncated,
    occupancyCellsSaturated: Boolean(base.occupancyCellsSaturated) || Boolean(extension.occupancyCellsSaturated),
    occupancyFineVoxelSizeMeters: fineOccupancyVoxelSize(base),
    occupancyDroppedCellUpdates: (base.occupancyDroppedCellUpdates ?? 0) + (extension.occupancyDroppedCellUpdates ?? 0),
    ...tsdf,
    manualControlPoints: annotations.manualControlPoints,
    operatorEdges: annotations.operatorEdges,
  };
  /* The two configured acquisition budgets are summed to cover their combined retained evidence. */
  snapshot.maxPointCapacity = Math.max(snapshot.maxPointCapacity, mergedPointCount, 1);
  snapshot.memoryBudgetPoints = Math.max(snapshot.memoryBudgetPoints, mergedPointCount, 1);
  return {
    ok: true,
    snapshot,
    diagnostics: { ...diagnostics, annotationsTruncated: annotations.annotationsTruncated },
    extensionToBase: { yaw: transform.yaw, tx: transform.tx, ty: transform.ty, tz: transform.tz },
  };
}

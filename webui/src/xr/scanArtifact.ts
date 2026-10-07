import type { ScanPointCloudSnapshot } from './webxrRuntime';

const MAGIC = new TextEncoder().encode('GXRSCAN1');
const FIXED_HEADER_BYTES = 20;
const SCHEMA_VERSION = 1;
export const MAX_SCAN_ARTIFACT_BYTES = 8 * 1024 * 1024;

const FLOAT_ARRAY_KEYS = [
  'positions', 'confidence', 'normals', 'tentativePositions', 'tentativeConfidence',
  'freeAirPositions', 'occupiedSurfacePositions', 'unknownCandidatePositions',
  'tsdfPositions', 'tsdfNormals', 'tsdfConfidence', 'tsdfResidualMeters',
  'tsdfNormalConfidence', 'tsdfCoverageConfidence',
] as const;
const BYTE_ARRAY_KEYS = ['tsdfCoverageReady'] as const;

type FloatArrayKey = (typeof FLOAT_ARRAY_KEYS)[number];
type ByteArrayKey = (typeof BYTE_ARRAY_KEYS)[number];
type BinaryArrayKey = FloatArrayKey | ByteArrayKey;
type SnapshotMetadata = Omit<ScanPointCloudSnapshot, BinaryArrayKey>;
type ProjectPoint = readonly [number, number, number];

interface ScanArtifactHeader {
  schema: number;
  metadata: Record<string, unknown>;
  lengths: Record<string, unknown>;
}

const METADATA_FIELDS = [
  'normalConfidence',
  'pointCount', 'tentativePointCount', 'rawPointCount', 'bounds', 'fullBounds',
  'voxelSizeMeters', 'maxDepthMeters', 'depthFrameCount', 'sampledDepthFrames',
  'capacityReached', 'maxPointCapacity', 'boundsTrimFraction', 'coverageCellCount',
  'coverageCellSizeMeters', 'adaptivePointsPerCell', 'memoryBudgetPoints',
  'candidateSampleCount', 'discardedSampleCount', 'replacementCount',
  'rebalanceEvictionCount', 'coverageRejectedCount', 'promotedPointCount',
  'readyCoverageCellCount', 'integrationKeyframes', 'polishFrames',
  'residualRejectedSamples', 'staleTentativePruned', 'averageStableResidualMeters',
  'occupancyVoxelSizeMeters', 'knownFreeVoxelCount', 'occupiedSurfaceVoxelCount',
  'unknownCandidateVoxelCount', 'occupancyCellsSaturated', 'occupancyFineVoxelSizeMeters',
  'occupancyDroppedCellUpdates', 'occupancyEnvelopeBounds',
  'occupancySolverCandidateBelowSurfaceMeters', 'occupancyEnvelopeComponentCount',
  'occupancyQuarantinedSurfaceComponentCount', 'occupancyQuarantinedSurfacePointCount',
  'occupancyBridgeRescanComponentCount', 'occupancyBridgeEvidenceTruncated',
  'tsdfPointCount', 'manualControlPoints', 'operatorEdges',
] as const;

// Schema-1 additive metadata is whitelisted; unknown top-level keys are ignored
// on decode. Older artifacts keep optional fields absent: normal-confidence
// consumers keep their local fallback, fine occupancy uses the coarse voxel,
// saturation falls back to false, dropped updates to zero, and candidate depth to zero.

function crc32(bytes: Uint8Array, start = 0): number {
  let crc = 0xffffffff;
  for (let index = start; index < bytes.length; index += 1) {
    crc ^= bytes[index];
    for (let bit = 0; bit < 8; bit += 1) {
      const mask = -(crc & 1);
      crc = (crc >>> 1) ^ (0xedb88320 & mask);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function invalidField(field: string, expectation: string): never {
  throw new Error('Invalid 3D scan artifact field ' + field + ': ' + expectation + '.');
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function hasOwn(record: Record<string, unknown>, key: string): boolean {
  return Object.prototype.hasOwnProperty.call(record, key);
}

function requiredValue(record: Record<string, unknown>, field: string): unknown {
  if (!hasOwn(record, field)) invalidField(field, 'is required');
  return record[field];
}

function counterField(record: Record<string, unknown>, field: string): number {
  const value = requiredValue(record, field);
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) {
    invalidField(field, 'must be a non-negative safe integer');
  }
  return value;
}

function finiteNumberField(record: Record<string, unknown>, field: string): number {
  const value = requiredValue(record, field);
  if (typeof value !== 'number' || !Number.isFinite(value)) invalidField(field, 'must be a finite number');
  return value;
}

function positiveNumberField(record: Record<string, unknown>, field: string): number {
  const value = finiteNumberField(record, field);
  if (value <= 0) invalidField(field, 'must be greater than zero');
  return value;
}

function booleanField(record: Record<string, unknown>, field: string): boolean {
  const value = requiredValue(record, field);
  if (typeof value !== 'boolean') invalidField(field, 'must be a boolean');
  return value;
}

function pointField(value: unknown, field: string): ProjectPoint {
  if (!Array.isArray(value) || value.length !== 3) invalidField(field, 'must contain exactly three coordinates');
  const coordinates: number[] = [];
  for (let index = 0; index < 3; index += 1) {
    const coordinate = value[index];
    if (typeof coordinate !== 'number' || !Number.isFinite(coordinate)) {
      invalidField(field + '[' + index + ']', 'must be a finite number');
    }
    coordinates.push(coordinate);
  }
  return [coordinates[0], coordinates[1], coordinates[2]];
}

function boundsField(
  record: Record<string, unknown>,
  field: 'bounds' | 'fullBounds' | 'occupancyEnvelopeBounds',
): ScanPointCloudSnapshot['bounds'] {
  const value = requiredValue(record, field);
  if (value === null) return null;
  if (!isRecord(value)) invalidField(field, 'must be null or a bounds object');
  const min = pointField(value.min, field + '.min');
  const max = pointField(value.max, field + '.max');
  for (let axis = 0; axis < 3; axis += 1) {
    if (min[axis] > max[axis]) invalidField(field, 'minimum coordinates must not exceed maximum coordinates');
  }
  return { min, max };
}

function typedFloatArray(record: Record<string, unknown>, field: FloatArrayKey | 'normalConfidence'): Float32Array {
  const value = requiredValue(record, field);
  if (!(value instanceof Float32Array)) invalidField(field, 'must be a Float32Array');
  return value;
}

function byteArray(record: Record<string, unknown>, field: ByteArrayKey): Uint8Array {
  const value = requiredValue(record, field);
  if (!(value instanceof Uint8Array)) invalidField(field, 'must be a Uint8Array');
  return value;
}

function validateConfidenceArray(values: Float32Array, field: string): void {
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (!Number.isFinite(value) || value < 0 || value > 1) {
      invalidField(field + '[' + index + ']', 'must be a finite number from zero through one');
    }
  }
}

function readNormalConfidenceMetadata(value: unknown, pointCount: number): Float32Array {
  if (!isRecord(value)) invalidField('normalConfidence', 'must be a Float32Array metadata object');
  const keys = Object.keys(value);
  if (keys.length !== pointCount) invalidField('normalConfidence', 'length must equal pointCount');
  const result = new Float32Array(pointCount);
  for (let index = 0; index < pointCount; index += 1) {
    const key = String(index);
    if (!hasOwn(value, key)) invalidField('normalConfidence[' + index + ']', 'is required');
    const confidence = value[key];
    if (typeof confidence !== 'number' || !Number.isFinite(confidence) || confidence < 0 || confidence > 1) {
      invalidField('normalConfidence[' + index + ']', 'must be a finite number from zero through one');
    }
    result[index] = confidence;
  }
  return result;
}

function controlPointsField(value: unknown): ScanPointCloudSnapshot['manualControlPoints'] {
  if (!Array.isArray(value)) invalidField('manualControlPoints', 'must be an array');
  const ids = new Set<number>();
  return value.map((entry, index) => {
    const field = 'manualControlPoints[' + index + ']';
    if (!isRecord(entry)) invalidField(field, 'must be an object');
    const id = entry.id;
    if (typeof id !== 'number' || !Number.isSafeInteger(id) || id <= 0) {
      invalidField(field + '.id', 'must be a positive safe integer');
    }
    if (ids.has(id)) invalidField(field + '.id', 'must be unique');
    ids.add(id);
    return { id, position: pointField(entry.position, field + '.position') };
  });
}

function operatorEdgesField(value: unknown): ScanPointCloudSnapshot['operatorEdges'] {
  if (!Array.isArray(value)) invalidField('operatorEdges', 'must be an array');
  const ids = new Set<number>();
  return value.map((entry, index) => {
    const field = 'operatorEdges[' + index + ']';
    if (!isRecord(entry)) invalidField(field, 'must be an object');
    const id = entry.id;
    if (typeof id !== 'number' || !Number.isSafeInteger(id) || id <= 0) {
      invalidField(field + '.id', 'must be a positive safe integer');
    }
    if (ids.has(id)) invalidField(field + '.id', 'must be unique');
    ids.add(id);

    const start = pointField(entry.start, field + '.start');
    const end = pointField(entry.end, field + '.end');
    if (Math.hypot(start[0] - end[0], start[1] - end[1], start[2] - end[2]) < 0.05) {
      invalidField(field, 'edge endpoints must be at least 0.05 meters apart');
    }
    const confidence = entry.confidence;
    if (typeof confidence !== 'number' || !Number.isFinite(confidence) || confidence < 0 || confidence > 1) {
      invalidField(field + '.confidence', 'must be a finite number from zero through one');
    }
    return { id, start, end, confidence };
  });
}

function optionalCounterField(record: Record<string, unknown>, field: string): number | undefined {
  if (!hasOwn(record, field) || record[field] === undefined) return undefined;
  return counterField(record, field);
}

function optionalFiniteNumberField(
  record: Record<string, unknown>,
  field: string,
  nonNegative: boolean,
): number | undefined {
  if (!hasOwn(record, field) || record[field] === undefined) return undefined;
  const value = finiteNumberField(record, field);
  if (nonNegative && value < 0) invalidField(field, 'must not be negative');
  return value;
}

function optionalBooleanField(record: Record<string, unknown>, field: string): boolean | undefined {
  if (!hasOwn(record, field) || record[field] === undefined) return undefined;
  return booleanField(record, field);
}

function validateSnapshotShape(snapshot: ScanPointCloudSnapshot): void {
  if (snapshot.positions.length !== snapshot.pointCount * 3) invalidField('positions', 'length must equal pointCount * 3');
  if (snapshot.normals.length !== snapshot.pointCount * 3) invalidField('normals', 'length must equal pointCount * 3');
  if (snapshot.confidence.length !== snapshot.pointCount) invalidField('confidence', 'length must equal pointCount');
  if (snapshot.normalConfidence !== undefined && snapshot.normalConfidence.length !== snapshot.pointCount) {
    invalidField('normalConfidence', 'length must equal pointCount');
  }
  if (snapshot.tentativePositions.length !== snapshot.tentativePointCount * 3) {
    invalidField('tentativePositions', 'length must equal tentativePointCount * 3');
  }
  if (snapshot.tentativeConfidence.length !== snapshot.tentativePointCount) {
    invalidField('tentativeConfidence', 'length must equal tentativePointCount');
  }
  if (snapshot.tsdfPositions.length !== snapshot.tsdfPointCount * 3) {
    invalidField('tsdfPositions', 'length must equal tsdfPointCount * 3');
  }
  if (snapshot.tsdfNormals.length !== snapshot.tsdfPointCount * 3) {
    invalidField('tsdfNormals', 'length must equal tsdfPointCount * 3');
  }
  if (snapshot.tsdfConfidence.length !== snapshot.tsdfPointCount) {
    invalidField('tsdfConfidence', 'length must equal tsdfPointCount');
  }
  if (snapshot.tsdfResidualMeters.length !== snapshot.tsdfPointCount) {
    invalidField('tsdfResidualMeters', 'length must equal tsdfPointCount');
  }
  if (snapshot.tsdfNormalConfidence.length !== snapshot.tsdfPointCount) {
    invalidField('tsdfNormalConfidence', 'length must equal tsdfPointCount');
  }
  if (snapshot.tsdfCoverageConfidence.length !== snapshot.tsdfPointCount) {
    invalidField('tsdfCoverageConfidence', 'length must equal tsdfPointCount');
  }
  if (snapshot.tsdfCoverageReady.length !== snapshot.tsdfPointCount) {
    invalidField('tsdfCoverageReady', 'length must equal tsdfPointCount');
  }
  if (snapshot.knownFreeVoxelCount !== snapshot.freeAirPositions.length / 3) {
    invalidField('knownFreeVoxelCount', 'must equal freeAirPositions.length / 3');
  }
  if (snapshot.occupiedSurfaceVoxelCount !== snapshot.occupiedSurfacePositions.length / 3) {
    invalidField('occupiedSurfaceVoxelCount', 'must equal occupiedSurfacePositions.length / 3');
  }
  if (snapshot.unknownCandidateVoxelCount !== snapshot.unknownCandidatePositions.length / 3) {
    invalidField('unknownCandidateVoxelCount', 'must equal unknownCandidatePositions.length / 3');
  }
  if (
    snapshot.occupancyFineVoxelSizeMeters !== undefined &&
    snapshot.occupancyFineVoxelSizeMeters > snapshot.occupancyVoxelSizeMeters
  ) {
    invalidField('occupancyFineVoxelSizeMeters', 'must not exceed occupancyVoxelSizeMeters');
  }

  for (const key of FLOAT_ARRAY_KEYS) {
    const values = snapshot[key];
    for (let index = 0; index < values.length; index += 1) {
      if (!Number.isFinite(values[index])) invalidField(key + '[' + index + ']', 'must be finite');
    }
  }
  if (snapshot.normalConfidence !== undefined) validateConfidenceArray(snapshot.normalConfidence, 'normalConfidence');
  validateConfidenceArray(snapshot.confidence, 'confidence');
  validateConfidenceArray(snapshot.tentativeConfidence, 'tentativeConfidence');
  validateConfidenceArray(snapshot.tsdfConfidence, 'tsdfConfidence');
  validateConfidenceArray(snapshot.tsdfNormalConfidence, 'tsdfNormalConfidence');
  validateConfidenceArray(snapshot.tsdfCoverageConfidence, 'tsdfCoverageConfidence');
  for (let index = 0; index < snapshot.tsdfCoverageReady.length; index += 1) {
    if (snapshot.tsdfCoverageReady[index] !== 0 && snapshot.tsdfCoverageReady[index] !== 1) {
      invalidField('tsdfCoverageReady[' + index + ']', 'must be zero or one');
    }
  }
}

function normalizeSnapshot(value: unknown): ScanPointCloudSnapshot {
  if (!isRecord(value)) invalidField('metadata', 'must be an object');

  const normalized: ScanPointCloudSnapshot = {
    positions: typedFloatArray(value, 'positions'),
    confidence: typedFloatArray(value, 'confidence'),
    normals: typedFloatArray(value, 'normals'),
    pointCount: counterField(value, 'pointCount'),
    tentativePositions: typedFloatArray(value, 'tentativePositions'),
    tentativeConfidence: typedFloatArray(value, 'tentativeConfidence'),
    tentativePointCount: counterField(value, 'tentativePointCount'),
    rawPointCount: counterField(value, 'rawPointCount'),
    bounds: boundsField(value, 'bounds'),
    fullBounds: boundsField(value, 'fullBounds'),
    voxelSizeMeters: positiveNumberField(value, 'voxelSizeMeters'),
    maxDepthMeters: positiveNumberField(value, 'maxDepthMeters'),
    depthFrameCount: counterField(value, 'depthFrameCount'),
    sampledDepthFrames: counterField(value, 'sampledDepthFrames'),
    capacityReached: booleanField(value, 'capacityReached'),
    maxPointCapacity: counterField(value, 'maxPointCapacity'),
    boundsTrimFraction: finiteNumberField(value, 'boundsTrimFraction'),
    coverageCellCount: counterField(value, 'coverageCellCount'),
    coverageCellSizeMeters: positiveNumberField(value, 'coverageCellSizeMeters'),
    adaptivePointsPerCell: counterField(value, 'adaptivePointsPerCell'),
    memoryBudgetPoints: counterField(value, 'memoryBudgetPoints'),
    candidateSampleCount: counterField(value, 'candidateSampleCount'),
    discardedSampleCount: counterField(value, 'discardedSampleCount'),
    replacementCount: counterField(value, 'replacementCount'),
    rebalanceEvictionCount: counterField(value, 'rebalanceEvictionCount'),
    coverageRejectedCount: counterField(value, 'coverageRejectedCount'),
    promotedPointCount: counterField(value, 'promotedPointCount'),
    readyCoverageCellCount: counterField(value, 'readyCoverageCellCount'),
    integrationKeyframes: counterField(value, 'integrationKeyframes'),
    polishFrames: counterField(value, 'polishFrames'),
    residualRejectedSamples: counterField(value, 'residualRejectedSamples'),
    staleTentativePruned: counterField(value, 'staleTentativePruned'),
    averageStableResidualMeters: finiteNumberField(value, 'averageStableResidualMeters'),
    occupancyVoxelSizeMeters: positiveNumberField(value, 'occupancyVoxelSizeMeters'),
    knownFreeVoxelCount: counterField(value, 'knownFreeVoxelCount'),
    occupiedSurfaceVoxelCount: counterField(value, 'occupiedSurfaceVoxelCount'),
    unknownCandidateVoxelCount: counterField(value, 'unknownCandidateVoxelCount'),
    freeAirPositions: typedFloatArray(value, 'freeAirPositions'),
    occupiedSurfacePositions: typedFloatArray(value, 'occupiedSurfacePositions'),
    unknownCandidatePositions: typedFloatArray(value, 'unknownCandidatePositions'),
    occupancyEnvelopeBounds: boundsField(value, 'occupancyEnvelopeBounds'),
    occupancyEnvelopeComponentCount: counterField(value, 'occupancyEnvelopeComponentCount'),
    occupancyQuarantinedSurfaceComponentCount: counterField(value, 'occupancyQuarantinedSurfaceComponentCount'),
    occupancyQuarantinedSurfacePointCount: counterField(value, 'occupancyQuarantinedSurfacePointCount'),
    occupancyBridgeRescanComponentCount: counterField(value, 'occupancyBridgeRescanComponentCount'),
    occupancyBridgeEvidenceTruncated: booleanField(value, 'occupancyBridgeEvidenceTruncated'),
    tsdfPositions: typedFloatArray(value, 'tsdfPositions'),
    tsdfNormals: typedFloatArray(value, 'tsdfNormals'),
    tsdfConfidence: typedFloatArray(value, 'tsdfConfidence'),
    tsdfResidualMeters: typedFloatArray(value, 'tsdfResidualMeters'),
    tsdfNormalConfidence: typedFloatArray(value, 'tsdfNormalConfidence'),
    tsdfCoverageConfidence: typedFloatArray(value, 'tsdfCoverageConfidence'),
    tsdfCoverageReady: byteArray(value, 'tsdfCoverageReady'),
    tsdfPointCount: counterField(value, 'tsdfPointCount'),
    manualControlPoints: controlPointsField(requiredValue(value, 'manualControlPoints')),
    operatorEdges: operatorEdgesField(requiredValue(value, 'operatorEdges')),
  };

  if (hasOwn(value, 'normalConfidence') && value.normalConfidence !== undefined) {
    normalized.normalConfidence = typedFloatArray(value, 'normalConfidence');
  }
  const occupancyCellsSaturated = optionalBooleanField(value, 'occupancyCellsSaturated');
  if (occupancyCellsSaturated !== undefined) normalized.occupancyCellsSaturated = occupancyCellsSaturated;
  const occupancyFineVoxelSizeMeters = optionalFiniteNumberField(value, 'occupancyFineVoxelSizeMeters', false);
  if (occupancyFineVoxelSizeMeters !== undefined) {
    if (occupancyFineVoxelSizeMeters <= 0) invalidField('occupancyFineVoxelSizeMeters', 'must be greater than zero');
    normalized.occupancyFineVoxelSizeMeters = occupancyFineVoxelSizeMeters;
  }
  const occupancyDroppedCellUpdates = optionalCounterField(value, 'occupancyDroppedCellUpdates');
  if (occupancyDroppedCellUpdates !== undefined) normalized.occupancyDroppedCellUpdates = occupancyDroppedCellUpdates;
  const occupancySolverCandidateBelowSurfaceMeters =
    optionalFiniteNumberField(value, 'occupancySolverCandidateBelowSurfaceMeters', true);
  if (occupancySolverCandidateBelowSurfaceMeters !== undefined) {
    normalized.occupancySolverCandidateBelowSurfaceMeters = occupancySolverCandidateBelowSurfaceMeters;
  }

  if (normalized.boundsTrimFraction < 0 || normalized.boundsTrimFraction > 1) {
    invalidField('boundsTrimFraction', 'must be from zero through one');
  }
  if (normalized.averageStableResidualMeters < 0) {
    invalidField('averageStableResidualMeters', 'must not be negative');
  }
  if (normalized.maxPointCapacity <= 0) invalidField('maxPointCapacity', 'must be greater than zero');
  if (normalized.adaptivePointsPerCell <= 0) invalidField('adaptivePointsPerCell', 'must be greater than zero');
  if (normalized.memoryBudgetPoints <= 0) invalidField('memoryBudgetPoints', 'must be greater than zero');

  validateSnapshotShape(normalized);
  return normalized;
}

function metadataAndArrays(snapshot: ScanPointCloudSnapshot, source: ScanPointCloudSnapshot): {
  metadata: SnapshotMetadata;
  arrays: Record<BinaryArrayKey, Float32Array | Uint8Array>;
} {
  const {
    positions, confidence, normals, tentativePositions, tentativeConfidence,
    freeAirPositions, occupiedSurfacePositions, unknownCandidatePositions,
    tsdfPositions, tsdfNormals, tsdfConfidence, tsdfResidualMeters,
    tsdfNormalConfidence, tsdfCoverageConfidence, tsdfCoverageReady,
    ...metadata
  } = snapshot;
  const canonicalMetadata = metadata as Record<string, unknown>;
  const orderedMetadata: Record<string, unknown> = {};
  for (const key of Object.keys(source)) {
    if (hasOwn(canonicalMetadata, key)) orderedMetadata[key] = canonicalMetadata[key];
  }
  for (const key of Object.keys(canonicalMetadata)) {
    if (!hasOwn(orderedMetadata, key)) orderedMetadata[key] = canonicalMetadata[key];
  }
  return {
    metadata: orderedMetadata as SnapshotMetadata,
    arrays: {
      positions, confidence, normals, tentativePositions, tentativeConfidence,
      freeAirPositions, occupiedSurfacePositions, unknownCandidatePositions,
      tsdfPositions, tsdfNormals, tsdfConfidence, tsdfResidualMeters,
      tsdfNormalConfidence, tsdfCoverageConfidence, tsdfCoverageReady,
    },
  };
}

export function encodeScanArtifact(snapshot: ScanPointCloudSnapshot): Uint8Array {
  const normalized = normalizeSnapshot(snapshot);
  const { metadata, arrays } = metadataAndArrays(normalized, snapshot);
  const lengths = {} as Record<BinaryArrayKey, number>;
  for (const key of FLOAT_ARRAY_KEYS) lengths[key] = arrays[key].length;
  for (const key of BYTE_ARRAY_KEYS) lengths[key] = arrays[key].length;

  const header = { schema: SCHEMA_VERSION, metadata, lengths };
  const headerBytes = new TextEncoder().encode(JSON.stringify(header));
  let payloadBytes = 0;
  for (const key of FLOAT_ARRAY_KEYS) payloadBytes += lengths[key] * Float32Array.BYTES_PER_ELEMENT;
  for (const key of BYTE_ARRAY_KEYS) payloadBytes += lengths[key];
  const totalBytes = FIXED_HEADER_BYTES + headerBytes.length + payloadBytes;
  if (totalBytes > MAX_SCAN_ARTIFACT_BYTES) {
    throw new Error('3D scan artifact is too large (' + totalBytes + ' bytes).');
  }

  const result = new Uint8Array(totalBytes);
  result.set(MAGIC, 0);
  const view = new DataView(result.buffer);
  view.setUint32(8, headerBytes.length, true);
  view.setUint32(12, payloadBytes, true);
  result.set(headerBytes, FIXED_HEADER_BYTES);

  let offset = FIXED_HEADER_BYTES + headerBytes.length;
  for (const key of FLOAT_ARRAY_KEYS) {
    const source = arrays[key] as Float32Array;
    const bytes = new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
    result.set(bytes, offset);
    offset += bytes.length;
  }
  for (const key of BYTE_ARRAY_KEYS) {
    const source = arrays[key] as Uint8Array;
    result.set(source, offset);
    offset += source.byteLength;
  }

  view.setUint32(16, crc32(result, FIXED_HEADER_BYTES), true);
  return result;
}

function parseHeader(bytes: Uint8Array, headerLength: number): ScanArtifactHeader {
  let parsed: unknown;
  try {
    parsed = JSON.parse(new TextDecoder().decode(bytes.subarray(FIXED_HEADER_BYTES, FIXED_HEADER_BYTES + headerLength)));
  }
  catch {
    throw new Error('3D scan artifact metadata is not valid JSON.');
  }
  if (!isRecord(parsed)) throw new Error('3D scan artifact metadata is invalid.');
  if (parsed.schema !== SCHEMA_VERSION) throw new Error('Unsupported 3D scan artifact schema.');
  if (!isRecord(parsed.metadata)) throw new Error('3D scan artifact metadata is missing.');
  if (!isRecord(parsed.lengths)) throw new Error('3D scan artifact array table is missing.');
  return { schema: SCHEMA_VERSION, metadata: parsed.metadata, lengths: parsed.lengths };
}

function checkedLength(value: unknown, key: BinaryArrayKey): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) {
    throw new Error('Invalid scan artifact array length for lengths.' + key + '.');
  }
  return value;
}

function decodeNormalConfidence(metadata: Record<string, unknown>): Float32Array | undefined {
  if (!hasOwn(metadata, 'normalConfidence')) return undefined;
  const pointCount = counterField(metadata, 'pointCount');
  return readNormalConfidenceMetadata(metadata.normalConfidence, pointCount);
}

function buildSnapshot(
  metadata: Record<string, unknown>,
  decoded: Record<BinaryArrayKey, Float32Array | Uint8Array>,
): ScanPointCloudSnapshot {
  const candidate: Record<string, unknown> = {};
  for (const key of METADATA_FIELDS) {
    if (hasOwn(metadata, key)) candidate[key] = metadata[key];
  }
  for (const key of FLOAT_ARRAY_KEYS) candidate[key] = decoded[key];
  for (const key of BYTE_ARRAY_KEYS) candidate[key] = decoded[key];
  const normalConfidence = decodeNormalConfidence(metadata);
  if (normalConfidence !== undefined) candidate.normalConfidence = normalConfidence;
  return normalizeSnapshot(candidate);
}

export function decodeScanArtifact(source: ArrayBuffer | Uint8Array): ScanPointCloudSnapshot {
  const bytes = source instanceof Uint8Array ? source : new Uint8Array(source);
  if (bytes.byteLength < FIXED_HEADER_BYTES || bytes.byteLength > MAX_SCAN_ARTIFACT_BYTES) {
    throw new Error('Invalid 3D scan artifact size ' + bytes.byteLength + '.');
  }
  for (let index = 0; index < MAGIC.length; index += 1) {
    if (bytes[index] !== MAGIC[index]) throw new Error('3D scan artifact magic/version mismatch.');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(8, true);
  const payloadLength = view.getUint32(12, true);
  const expectedCrc = view.getUint32(16, true);
  if (headerLength === 0) throw new Error('3D scan artifact length table is inconsistent.');
  if (crc32(bytes, FIXED_HEADER_BYTES) !== expectedCrc) {
    throw new Error('3D scan artifact checksum mismatch; saved scan is incomplete or corrupted.');
  }
  if (FIXED_HEADER_BYTES + headerLength + payloadLength !== bytes.byteLength) {
    throw new Error('3D scan artifact length table is inconsistent.');
  }

  const header = parseHeader(bytes, headerLength);
  let offset = FIXED_HEADER_BYTES + headerLength;
  const decoded = {} as Record<BinaryArrayKey, Float32Array | Uint8Array>;
  for (const key of FLOAT_ARRAY_KEYS) {
    const length = checkedLength(header.lengths[key], key);
    const byteLength = length * Float32Array.BYTES_PER_ELEMENT;
    if (byteLength > bytes.byteLength - offset) throw new Error('3D scan artifact ' + key + ' exceeds payload.');
    const copy = bytes.slice(offset, offset + byteLength);
    decoded[key] = new Float32Array(copy.buffer, copy.byteOffset, length);
    offset += byteLength;
  }
  for (const key of BYTE_ARRAY_KEYS) {
    const length = checkedLength(header.lengths[key], key);
    if (length > bytes.byteLength - offset) throw new Error('3D scan artifact ' + key + ' exceeds payload.');
    decoded[key] = bytes.slice(offset, offset + length);
    offset += length;
  }
  if (offset !== bytes.byteLength) throw new Error('3D scan artifact has trailing payload bytes.');

  return buildSnapshot(header.metadata, decoded);
}

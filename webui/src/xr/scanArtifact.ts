import type { ScanPointCloudSnapshot } from './webxrRuntime';

const MAGIC = new TextEncoder().encode('GXRSCAN1');
const FIXED_HEADER_BYTES = 20;
const SCHEMA_VERSION = 1;
export const MAX_SCAN_ARTIFACT_BYTES = 8 * 1024 * 1024;

const FLOAT_ARRAY_KEYS = [
  'positions',
  'confidence',
  'normals',
  'tentativePositions',
  'tentativeConfidence',
  'freeAirPositions',
  'occupiedSurfacePositions',
  'unknownCandidatePositions',
  'tsdfPositions',
  'tsdfNormals',
  'tsdfConfidence',
  'tsdfResidualMeters',
  'tsdfNormalConfidence',
  'tsdfCoverageConfidence',
] as const;
const BYTE_ARRAY_KEYS = ['tsdfCoverageReady'] as const;

type FloatArrayKey = (typeof FLOAT_ARRAY_KEYS)[number];
type ByteArrayKey = (typeof BYTE_ARRAY_KEYS)[number];
type BinaryArrayKey = FloatArrayKey | ByteArrayKey;

type SnapshotMetadata = Omit<ScanPointCloudSnapshot, BinaryArrayKey>;

interface ScanArtifactHeader {
  schema: number;
  metadata: SnapshotMetadata;
  lengths: Record<BinaryArrayKey, number>;
}

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

function checkedLength(value: unknown, key: string): number {
  if (!Number.isSafeInteger(value) || Number(value) < 0) {
    throw new Error(`Invalid scan artifact array length for ${key}.`);
  }
  return Number(value);
}

function assertFiniteArray(values: ArrayLike<number>, key: string): void {
  for (let index = 0; index < values.length; index += 1) {
    if (!Number.isFinite(Number(values[index]))) {
      throw new Error(`Non-finite value in scan artifact field ${key}.`);
    }
  }
}

function validateSnapshotShape(snapshot: ScanPointCloudSnapshot): void {
  if (snapshot.pointCount < 0 || snapshot.tentativePointCount < 0 || snapshot.tsdfPointCount < 0) {
    throw new Error('Negative point count in scan snapshot.');
  }
  if (snapshot.positions.length !== snapshot.pointCount * 3) throw new Error('Stable point positions/count mismatch.');
  if (snapshot.normals.length !== snapshot.pointCount * 3) throw new Error('Stable point normals/count mismatch.');
  if (snapshot.confidence.length !== snapshot.pointCount) throw new Error('Stable point confidence/count mismatch.');
  if (snapshot.tentativePositions.length !== snapshot.tentativePointCount * 3) throw new Error('Tentative point positions/count mismatch.');
  if (snapshot.tentativeConfidence.length !== snapshot.tentativePointCount) throw new Error('Tentative point confidence/count mismatch.');
  if (snapshot.tsdfPositions.length !== snapshot.tsdfPointCount * 3) throw new Error('TSDF point positions/count mismatch.');
  if (snapshot.tsdfNormals.length !== snapshot.tsdfPointCount * 3) throw new Error('TSDF point normals/count mismatch.');
  if (snapshot.tsdfConfidence.length !== snapshot.tsdfPointCount) throw new Error('TSDF point confidence/count mismatch.');
  if (snapshot.tsdfResidualMeters.length !== snapshot.tsdfPointCount) throw new Error('TSDF residual/count mismatch.');
  if (snapshot.tsdfNormalConfidence.length !== snapshot.tsdfPointCount) throw new Error('TSDF normal-confidence/count mismatch.');
  if (snapshot.tsdfCoverageConfidence.length !== snapshot.tsdfPointCount) throw new Error('TSDF coverage-confidence/count mismatch.');
  if (snapshot.tsdfCoverageReady.length !== snapshot.tsdfPointCount) throw new Error('TSDF coverage-ready/count mismatch.');
  for (const key of FLOAT_ARRAY_KEYS) assertFiniteArray(snapshot[key], key);
}

function metadataAndArrays(snapshot: ScanPointCloudSnapshot): {
  metadata: SnapshotMetadata;
  arrays: Record<BinaryArrayKey, Float32Array | Uint8Array>;
} {
  const {
    positions,
    confidence,
    normals,
    tentativePositions,
    tentativeConfidence,
    freeAirPositions,
    occupiedSurfacePositions,
    unknownCandidatePositions,
    tsdfPositions,
    tsdfNormals,
    tsdfConfidence,
    tsdfResidualMeters,
    tsdfNormalConfidence,
    tsdfCoverageConfidence,
    tsdfCoverageReady,
    ...metadata
  } = snapshot;
  return {
    metadata,
    arrays: {
      positions,
      confidence,
      normals,
      tentativePositions,
      tentativeConfidence,
      freeAirPositions,
      occupiedSurfacePositions,
      unknownCandidatePositions,
      tsdfPositions,
      tsdfNormals,
      tsdfConfidence,
      tsdfResidualMeters,
      tsdfNormalConfidence,
      tsdfCoverageConfidence,
      tsdfCoverageReady,
    },
  };
}

export function encodeScanArtifact(snapshot: ScanPointCloudSnapshot): Uint8Array {
  validateSnapshotShape(snapshot);
  const { metadata, arrays } = metadataAndArrays(snapshot);
  const lengths = {} as Record<BinaryArrayKey, number>;
  for (const key of FLOAT_ARRAY_KEYS) lengths[key] = arrays[key].length;
  for (const key of BYTE_ARRAY_KEYS) lengths[key] = arrays[key].length;

  const header: ScanArtifactHeader = { schema: SCHEMA_VERSION, metadata, lengths };
  const headerBytes = new TextEncoder().encode(JSON.stringify(header));
  let payloadBytes = 0;
  for (const key of FLOAT_ARRAY_KEYS) payloadBytes += lengths[key] * Float32Array.BYTES_PER_ELEMENT;
  for (const key of BYTE_ARRAY_KEYS) payloadBytes += lengths[key];
  const totalBytes = FIXED_HEADER_BYTES + headerBytes.length + payloadBytes;
  if (totalBytes > MAX_SCAN_ARTIFACT_BYTES) {
    throw new Error(`3D scan artifact is too large (${totalBytes} bytes).`);
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
  if (typeof parsed !== 'object' || parsed === null) throw new Error('3D scan artifact metadata is invalid.');
  const candidate = parsed as Partial<ScanArtifactHeader>;
  if (candidate.schema !== SCHEMA_VERSION) throw new Error(`Unsupported 3D scan artifact schema ${String(candidate.schema)}.`);
  if (typeof candidate.metadata !== 'object' || candidate.metadata === null) throw new Error('3D scan artifact metadata is missing.');
  if (typeof candidate.lengths !== 'object' || candidate.lengths === null) throw new Error('3D scan artifact array table is missing.');
  return candidate as ScanArtifactHeader;
}

export function decodeScanArtifact(source: ArrayBuffer | Uint8Array): ScanPointCloudSnapshot {
  const bytes = source instanceof Uint8Array ? source : new Uint8Array(source);
  if (bytes.byteLength < FIXED_HEADER_BYTES || bytes.byteLength > MAX_SCAN_ARTIFACT_BYTES) {
    throw new Error(`Invalid 3D scan artifact size ${bytes.byteLength}.`);
  }
  for (let index = 0; index < MAGIC.length; index += 1) {
    if (bytes[index] !== MAGIC[index]) throw new Error('3D scan artifact magic/version mismatch.');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(8, true);
  const payloadLength = view.getUint32(12, true);
  const expectedCrc = view.getUint32(16, true);
  if (headerLength === 0 || FIXED_HEADER_BYTES + headerLength + payloadLength !== bytes.byteLength) {
    throw new Error('3D scan artifact length table is inconsistent.');
  }
  if (crc32(bytes, FIXED_HEADER_BYTES) !== expectedCrc) {
    throw new Error('3D scan artifact checksum mismatch; saved scan is incomplete or corrupted.');
  }

  const header = parseHeader(bytes, headerLength);
  let offset = FIXED_HEADER_BYTES + headerLength;
  const decoded = {} as Record<BinaryArrayKey, Float32Array | Uint8Array>;
  for (const key of FLOAT_ARRAY_KEYS) {
    const length = checkedLength(header.lengths[key], key);
    const byteLength = length * Float32Array.BYTES_PER_ELEMENT;
    if (offset + byteLength > bytes.byteLength) throw new Error(`3D scan artifact ${key} exceeds payload.`);
    const copy = bytes.slice(offset, offset + byteLength);
    decoded[key] = new Float32Array(copy.buffer, copy.byteOffset, length);
    offset += byteLength;
  }
  for (const key of BYTE_ARRAY_KEYS) {
    const length = checkedLength(header.lengths[key], key);
    if (offset + length > bytes.byteLength) throw new Error(`3D scan artifact ${key} exceeds payload.`);
    decoded[key] = bytes.slice(offset, offset + length);
    offset += length;
  }
  if (offset !== bytes.byteLength) throw new Error('3D scan artifact has trailing payload bytes.');

  const snapshot = {
    ...header.metadata,
    ...decoded,
  } as ScanPointCloudSnapshot;
  validateSnapshotShape(snapshot);
  return snapshot;
}

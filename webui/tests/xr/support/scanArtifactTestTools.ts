const FIXED_HEADER_BYTES = 20;

function crc32(bytes: Uint8Array, start: number): number {
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

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function rewriteScanArtifactMetadata(
  encoded: Uint8Array,
  update: (metadata: Record<string, unknown>) => void,
): Uint8Array {
  const originalView = new DataView(encoded.buffer, encoded.byteOffset, encoded.byteLength);
  const headerLength = originalView.getUint32(8, true);
  const headerStart = FIXED_HEADER_BYTES;
  const payloadStart = headerStart + headerLength;
  const parsed: unknown = JSON.parse(new TextDecoder().decode(encoded.subarray(headerStart, payloadStart)));
  if (!record(parsed) || !record(parsed.metadata)) throw new Error('Test artifact header has no metadata object.');
  const metadata = { ...parsed.metadata };
  update(metadata);
  const header = { ...parsed, metadata };
  const headerBytes = new TextEncoder().encode(JSON.stringify(header));
  const payload = encoded.subarray(payloadStart);
  const result = new Uint8Array(FIXED_HEADER_BYTES + headerBytes.length + payload.length);
  result.set(encoded.subarray(0, 8), 0);
  const view = new DataView(result.buffer);
  view.setUint32(8, headerBytes.length, true);
  view.setUint32(12, payload.length, true);
  result.set(headerBytes, FIXED_HEADER_BYTES);
  result.set(payload, FIXED_HEADER_BYTES + headerBytes.length);
  view.setUint32(16, crc32(result, FIXED_HEADER_BYTES), true);
  return result;
}

const LEGACY_FLOAT_ARRAY_KEYS = [
  'positions', 'confidence', 'normals', 'tentativePositions', 'tentativeConfidence',
  'freeAirPositions', 'occupiedSurfacePositions', 'unknownCandidatePositions',
  'tsdfPositions', 'tsdfNormals', 'tsdfConfidence', 'tsdfResidualMeters',
  'tsdfNormalConfidence', 'tsdfCoverageConfidence',
] as const;

/** Rebuild the historical schema-1 JSON-object encoding of normalConfidence. */
export function rewriteNormalConfidenceAsLegacy(
  encoded: Uint8Array,
  includeNormalConfidence: boolean,
): Uint8Array {
  const view = new DataView(encoded.buffer, encoded.byteOffset, encoded.byteLength);
  const headerLength = view.getUint32(8, true);
  const headerStart = FIXED_HEADER_BYTES;
  const payloadStart = headerStart + headerLength;
  const parsed: unknown = JSON.parse(new TextDecoder().decode(encoded.subarray(headerStart, payloadStart)));
  if (!record(parsed) || !record(parsed.metadata) || !record(parsed.lengths)) {
    throw new Error('Test artifact header is incomplete.');
  }
  const header = { ...parsed, metadata: { ...parsed.metadata }, lengths: { ...parsed.lengths } };
  const hasBinaryNormalConfidence = Object.prototype.hasOwnProperty.call(header.lengths, 'normalConfidence');
  const normalLength = Number(header.lengths.normalConfidence ?? 0);
  if (hasBinaryNormalConfidence) {
    let normalOffset = payloadStart;
    for (const key of LEGACY_FLOAT_ARRAY_KEYS) normalOffset += Number(header.lengths[key] ?? 0) * Float32Array.BYTES_PER_ELEMENT;
    const normalBytes = encoded.subarray(normalOffset, normalOffset + normalLength * Float32Array.BYTES_PER_ELEMENT);
    if (includeNormalConfidence) {
      const values = new Float32Array(normalLength);
      for (let index = 0; index < normalLength; index += 1) {
        values[index] = new DataView(normalBytes.buffer, normalBytes.byteOffset + index * 4, 4).getFloat32(0, true);
      }
      header.metadata.normalConfidence = Object.fromEntries(Array.from(values, (value, index) => [String(index), value]));
    } else {
      delete header.metadata.normalConfidence;
    }
    delete header.lengths.normalConfidence;
    const before = encoded.subarray(payloadStart, normalOffset);
    const after = encoded.subarray(normalOffset + normalLength * Float32Array.BYTES_PER_ELEMENT);
    const payload = new Uint8Array(before.length + after.length);
    payload.set(before, 0);
    payload.set(after, before.length);
    const headerBytes = new TextEncoder().encode(JSON.stringify(header));
    const result = new Uint8Array(FIXED_HEADER_BYTES + headerBytes.length + payload.length);
    result.set(encoded.subarray(0, 8), 0);
    const resultView = new DataView(result.buffer);
    resultView.setUint32(8, headerBytes.length, true);
    resultView.setUint32(12, payload.length, true);
    result.set(headerBytes, FIXED_HEADER_BYTES);
    result.set(payload, FIXED_HEADER_BYTES + headerBytes.length);
    resultView.setUint32(16, crc32(result, FIXED_HEADER_BYTES), true);
    return result;
  }
  return rewriteScanArtifactMetadata(encoded, (metadata) => {
    if (!includeNormalConfidence) delete metadata.normalConfidence;
  });
}

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

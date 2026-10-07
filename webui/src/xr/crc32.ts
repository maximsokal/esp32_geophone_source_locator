const CRC32_TABLE = new Uint32Array(256);

for (let value = 0; value < CRC32_TABLE.length; value += 1) {
  let remainder = value;
  for (let bit = 0; bit < 8; bit += 1) {
    remainder = (remainder & 1) !== 0 ? (remainder >>> 1) ^ 0xedb88320 : remainder >>> 1;
  }
  CRC32_TABLE[value] = remainder >>> 0;
}

export function crc32Update(state: number, bytes: Uint8Array, start = 0, end = bytes.length): number {
  let crc = state >>> 0;
  for (let index = start; index < end; index += 1) {
    crc = CRC32_TABLE[(crc ^ bytes[index]) & 0xff] ^ (crc >>> 8);
  }
  return crc >>> 0;
}

export function crc32Finalize(state: number): number {
  return (state ^ 0xffffffff) >>> 0;
}

export function crc32(bytes: Uint8Array, start = 0): number {
  return crc32Finalize(crc32Update(0xffffffff, bytes, start));
}

/** Compute a whole-buffer CRC and a suffix CRC in one pass over the input. */
export function crc32Pair(bytes: Uint8Array, suffixStart: number): readonly [number, number] {
  let whole = 0xffffffff;
  let suffix = 0xffffffff;
  for (let index = 0; index < bytes.length; index += 1) {
    const value = bytes[index];
    whole = CRC32_TABLE[(whole ^ value) & 0xff] ^ (whole >>> 8);
    if (index >= suffixStart) suffix = CRC32_TABLE[(suffix ^ value) & 0xff] ^ (suffix >>> 8);
  }
  return [crc32Finalize(whole), crc32Finalize(suffix)];
}

/** Builds a GEOSEIS1 v2 file exactly as components/seismic_recorder writes it. */
export function v2File(options: {
  rate: number;
  scale?: number[];
  offset?: number[];
  blocks: Array<{ firstIndex: number; firstTimestampUs: number; codes: number[][]; gap?: boolean }>;
}): ArrayBuffer {
  const scale = options.scale ?? [1, 1, 1, 1];
  const offset = options.offset ?? [0, 0, 0, 0];
  const payload = options.blocks.reduce((sum, block) => sum + 20 + block.codes.length * 8, 0);
  const bytes = new ArrayBuffer(24 + 44 + payload);
  const view = new DataView(bytes);
  'GEOSEIS1'.split('').forEach((character, i) => view.setUint8(i, character.charCodeAt(0)));
  view.setUint16(8, 2, true);
  view.setUint16(10, 4, true);
  view.setUint32(12, options.rate, true);
  view.setUint32(16, 8, true);
  for (let channel = 0; channel < 4; channel += 1) {
    view.setFloat32(24 + channel * 4, scale[channel], true);
    view.setFloat32(40 + channel * 4, offset[channel], true);
  }
  view.setFloat32(56, 5, true);
  view.setUint32(60, 1, true);
  view.setUint32(64, 20_000_000, true);
  let at = 68;
  for (const block of options.blocks) {
    view.setBigUint64(at, BigInt(block.firstIndex), true);
    view.setBigUint64(at + 8, BigInt(block.firstTimestampUs), true);
    view.setUint16(at + 16, block.codes.length, true);
    view.setUint16(at + 18, block.gap ? 1 : 0, true);
    at += 20;
    for (const frame of block.codes) {
      for (const code of frame) { view.setInt16(at, code, true); at += 2; }
    }
  }
  return bytes;
}

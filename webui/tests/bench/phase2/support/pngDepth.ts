import { inflateSync } from 'node:zlib';
import { readFileSync } from 'node:fs';

/**
 * Minimal decoder for the 16-bit (and 8-bit) greyscale PNGs of the TUM RGB-D, ICL-NUIM and Redwood depth sequences. Non-interlaced only, which is what all
 * three datasets use. Test/bench tooling: never bundled.
 */
export interface DepthPng { width: number; height: number; values: Uint16Array }

function paeth(a: number, b: number, c: number): number {
  const p = a + b - c;
  const pa = Math.abs(p - a); const pb = Math.abs(p - b); const pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

export function decodeDepthPng(bytes: Uint8Array): DepthPng {
  const signature = [137, 80, 78, 71, 13, 10, 26, 10];
  for (let i = 0; i < 8; i += 1) if (bytes[i] !== signature[i]) throw new Error('not a PNG');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let width = 0; let height = 0; let bitDepth = 0; let colorType = 0; let interlace = 0;
  const idat: Uint8Array[] = [];
  for (let offset = 8; offset + 8 <= bytes.length;) {
    const length = view.getUint32(offset);
    const type = String.fromCharCode(bytes[offset + 4], bytes[offset + 5], bytes[offset + 6], bytes[offset + 7]);
    const data = bytes.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = view.getUint32(offset + 8); height = view.getUint32(offset + 12);
      bitDepth = bytes[offset + 16]; colorType = bytes[offset + 17]; interlace = bytes[offset + 20];
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    offset += 12 + length;
  }
  if (colorType !== 0 || (bitDepth !== 16 && bitDepth !== 8) || interlace !== 0) throw new Error(`unsupported PNG (colorType ${colorType}, depth ${bitDepth}, interlace ${interlace})`);
  const raw = inflateSync(Buffer.concat(idat));
  const bpp = bitDepth / 8;
  const stride = width * bpp;
  const out = new Uint8Array(height * stride);
  for (let y = 0; y < height; y += 1) {
    const filter = raw[y * (stride + 1)];
    const src = y * (stride + 1) + 1;
    const dst = y * stride;
    for (let x = 0; x < stride; x += 1) {
      const left = x >= bpp ? out[dst + x - bpp] : 0;
      const up = y > 0 ? out[dst - stride + x] : 0;
      const upLeft = y > 0 && x >= bpp ? out[dst - stride + x - bpp] : 0;
      const v = raw[src + x];
      out[dst + x] = (filter === 0 ? v : filter === 1 ? v + left : filter === 2 ? v + up : filter === 3 ? v + ((left + up) >> 1) : v + paeth(left, up, upLeft)) & 255;
    }
  }
  const values = new Uint16Array(width * height);
  for (let i = 0; i < values.length; i += 1) values[i] = bitDepth === 16 ? (out[i * 2] << 8) | out[i * 2 + 1] : out[i];
  return { width, height, values };
}

export function readDepthPng(path: string): DepthPng { return decodeDepthPng(readFileSync(path)); }

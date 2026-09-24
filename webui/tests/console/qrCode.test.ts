import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { encodeQr, qrByteCapacity, qrSvgPath } from '../../src/domain/qrCode';

/*
 * MAX-150: the QR encoder must match the `segno` reference encoder module
 * for module (fixtures generated once with segno 1.6.6, byte mode, level M,
 * every mask 0..7).
 */

interface Golden { text: string; version: number; size: number; masks: string[] }
const golden: Golden[] = JSON.parse(readFileSync(fileURLToPath(new URL('./fixtures/qr_golden.json', import.meta.url)), 'utf-8'));

function unpack(hex: string, size: number): string {
  const bits = [...hex].map((c) => parseInt(c, 16).toString(2).padStart(4, '0')).join('');
  return bits.slice(0, size * size);
}

function flatten(modules: boolean[][]): string {
  return modules.map((row) => row.map((m) => (m ? '1' : '0')).join('')).join('');
}

describe('QR encoder', () => {
  for (const g of golden) {
    it(`matches the reference for "${g.text.slice(0, 24)}…" (version ${g.version}) with every mask`, () => {
      for (let mask = 0; mask < 8; mask += 1) {
        const code = encodeQr(g.text, mask);
        expect(code).not.toBeNull();
        expect(code?.version).toBe(g.version);
        expect(code?.size).toBe(g.size);
        expect(flatten(code!.modules)).toBe(unpack(g.masks[mask], g.size));
      }
      /* The automatically chosen mask is one of the valid reference codes. */
      const auto = encodeQr(g.text);
      expect(flatten(auto!.modules)).toBe(unpack(g.masks[auto!.mask], g.size));
    });
  }

  it('refuses payloads beyond version 10 instead of truncating', () => {
    expect(qrByteCapacity(10)).toBe(213);
    expect(qrByteCapacity(1)).toBe(14);
    expect(encodeQr('z'.repeat(213))?.version).toBe(10);
    expect(encodeQr('z'.repeat(214))).toBeNull();
  });

  it('renders an SVG path with a quiet zone', () => {
    const code = encodeQr('HELLO')!;
    const path = qrSvgPath(code, 4);
    /* Top-left finder corner module sits at (4,4). */
    expect(path.startsWith('M4,4h1v1h-1z')).toBe(true);
  });
});

import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { sha256Hex } from '../../src/xr/sha256';

const reference = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');

describe('sha256Hex (issue #34)', () => {
  it('matches the FIPS 180-4 test vectors', () => {
    const text = (value: string) => new TextEncoder().encode(value);
    expect(sha256Hex(new Uint8Array(0))).toBe('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
    expect(sha256Hex(text('abc'))).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    expect(sha256Hex(text('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))).toBe('248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
    expect(sha256Hex(text('123456789'))).toBe('15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225');
  });

  it('agrees with node:crypto around every padding boundary and on a large random buffer', () => {
    let state = 0x2545f491;
    const next = (): number => { state ^= state << 13; state >>>= 0; state ^= state >>> 17; state ^= state << 5; state >>>= 0; return state; };
    for (const length of [0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 1000]) {
      const bytes = new Uint8Array(length);
      for (let i = 0; i < length; i += 1) bytes[i] = next() & 0xff;
      expect(sha256Hex(bytes)).toBe(reference(bytes));
    }
    const large = new Uint8Array(1_000_003);
    for (let i = 0; i < large.length; i += 1) large[i] = next() & 0xff;
    expect(sha256Hex(large)).toBe(reference(large));
  });

  it('hashes a view without reading outside it', () => {
    const backing = new Uint8Array(200).fill(0xaa);
    const view = backing.subarray(10, 75);
    expect(sha256Hex(view)).toBe(reference(Uint8Array.from(view)));
  });
});

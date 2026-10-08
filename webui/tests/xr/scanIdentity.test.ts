import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it } from 'vitest';

import { crc32 } from '../../src/xr/crc32';
import { createScanIdentity, getScanIdentityMetrics, resetScanIdentityMetrics } from '../../src/xr/scanIdentity';

const OCCUPANCY_GOLDEN_PATH = resolve(__dirname, '../../../tests/vectors/scan_occupancy_v2.bin');

describe('shared CRC32 and scan identity', () => {
  it('matches the standard CRC32 check vector', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
  });

  it('matches the stored checksum of an existing golden artifact', () => {
    const golden = new Uint8Array(readFileSync(OCCUPANCY_GOLDEN_PATH));
    const expected = new DataView(golden.buffer, golden.byteOffset, golden.byteLength).getUint32(16, true);

    expect(crc32(golden, 20)).toBe(expected);
  });

  it('creates one immutable frame identity and counts one whole-scan CRC', () => {
    resetScanIdentityMetrics();
    const identity = createScanIdentity(new TextEncoder().encode('123456789'));

    expect(identity).toEqual({ byteLength: 9, crc32: 0xcbf43926, frameId: 'scan-crc32:cbf43926' });
    expect(Object.isFrozen(identity)).toBe(true);
    expect(getScanIdentityMetrics().wholeScanCrcInvocations).toBe(1);
  });
});

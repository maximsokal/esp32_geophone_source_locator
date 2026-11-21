import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { decodeScanArtifact, decodeScanArtifactWithIdentity, encodeScanArtifactWithIdentity } from '../../src/xr/scanArtifact';
import { makeValidScanSnapshot } from './support/validScanSnapshot';

/*
 * Issue #34. The scan identity ("scan-crc32:xxxxxxxx") is a 32-bit CRC of the whole file: good against accidental corruption, but two different
 * valid scans can share it. This pair was found once (deterministic xorshift32 search, 44 687 tries) and is pinned here, so the property is a
 * fixed fact in the suite and not a probabilistic claim. Everything that binds a sidecar to a scan by this id alone (registration, occupancy,
 * preview, tags, the C identity cache) cannot tell A from B. These tests CHARACTERIZE that legacy contract; they must be changed, not
 * deleted, when a collision-resistant identity replaces it (the strong-identity test below is the open requirement).
 */
const POINT_A: readonly [number, number, number] = [1.0394198894500732, 1.079842448234558, 1];
const POINT_B: readonly [number, number, number] = [1.1354225873947144, 1.1363348960876465, 1];
const COLLIDING_ID = 'scan-crc32:05897d97';

function scanAt(point: readonly [number, number, number]) {
  const base = makeValidScanSnapshot();
  return {
    ...base,
    positions: new Float32Array(point),
    bounds: { min: [0, 0, 0] as const, max: [3, 3, 3] as const },
    fullBounds: { min: [0, 0, 0] as const, max: [3, 3, 3] as const },
    occupiedSurfacePositions: new Float32Array(point),
  };
}

const sha256 = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');

describe('legacy scan identity is a 32-bit CRC: a pinned collision (issue #34)', () => {
  const a = encodeScanArtifactWithIdentity(scanAt(POINT_A));
  const b = encodeScanArtifactWithIdentity(scanAt(POINT_B));

  it('two different, fully valid scans decode successfully', () => {
    const decodedA = decodeScanArtifact(a.bytes);
    const decodedB = decodeScanArtifact(b.bytes);
    expect(Array.from(decodedA.positions)).toEqual([...POINT_A]);
    expect(Array.from(decodedB.positions)).toEqual([...POINT_B]);
    expect(Array.from(decodedA.positions)).not.toEqual(Array.from(decodedB.positions));
    expect(Math.hypot(POINT_A[0] - POINT_B[0], POINT_A[1] - POINT_B[1])).toBeGreaterThan(0.1);   /* ~0.11 m apart in the plane */
  });

  it('the files are different bytes of the same length', () => {
    expect(a.bytes.byteLength).toBe(b.bytes.byteLength);
    expect(Buffer.from(a.bytes).equals(Buffer.from(b.bytes))).toBe(false);
  });

  it('but the legacy identity cannot tell them apart: same scan-crc32 and same length', () => {
    expect(a.identity.frameId).toBe(COLLIDING_ID);
    expect(b.identity.frameId).toBe(COLLIDING_ID);
    expect(a.identity.byteLength).toBe(b.identity.byteLength);
  });

  it('the scan identity itself now carries a SHA-256 that tells them apart', () => {
    expect(a.identity.sha256).toMatch(/^[0-9a-f]{64}$/);
    expect(a.identity.sha256).toBe(sha256(a.bytes));
    expect(a.identity.sha256).not.toBe(b.identity.sha256);
    expect(decodeScanArtifactWithIdentity(a.bytes).identity.sha256).toBe(a.identity.sha256);   /* the restore path reproduces it */
  });

  it('an independent content digest does tell them apart (what a strong identity must carry)', () => {
    expect(sha256(a.bytes)).not.toBe(sha256(b.bytes));
    expect(sha256(a.bytes)).toBe(sha256(Uint8Array.from(a.bytes)));                              /* identical bytes hash identically */
  });

  /* The device side of the same requirement is covered by tests/host/web_scan_store_http_test.c (a derivative bound to A's SHA-256 is refused for B). */
});

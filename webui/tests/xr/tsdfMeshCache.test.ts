import { describe, expect, it } from 'vitest';
import { TsdfMeshCache, tsdfGeometryFingerprint, tsdfSettingsKey, type TsdfFingerprintSource } from '../../src/xr/tsdfMeshCache';

function geometry(overrides: Partial<TsdfFingerprintSource> = {}): TsdfFingerprintSource {
  return {
    tsdfPositions: Float32Array.from([0, 0, 0, 0.1, 0, 0, 0, 0.1, 0.2]),
    tsdfNormals: Float32Array.from([0, 0, 1, 0, 0, 1, 0, 0, 1]),
    tsdfCoverageConfidence: Float32Array.from([0.9, 0.8, 0.7]),
    freeAirPositions: Float32Array.from([1, 1, 1]),
    voxelSizeMeters: 0.04,
    tsdfPointCount: 3,
    occupancyFineVoxelSizeMeters: 0.06,
    ...overrides,
  };
}

describe('TSDF geometry fingerprint: the identity of what the reconstruction is computed from', () => {
  it('CACHE-03 the same content in a new object (the scan decoded again for another viewer) has the same fingerprint', () => {
    const a = geometry();
    const b = geometry({ tsdfPositions: a.tsdfPositions.slice(), tsdfNormals: a.tsdfNormals.slice() });
    expect(tsdfGeometryFingerprint(b)).toBe(tsdfGeometryFingerprint(a));
  });

  it('CACHE-04 any change of the geometry fed to the reconstruction changes it', () => {
    const base = tsdfGeometryFingerprint(geometry());
    const moved = geometry(); moved.tsdfPositions[4] += 0.001;
    const turned = geometry(); turned.tsdfNormals[2] = 0.99;
    const confident = geometry(); confident.tsdfCoverageConfidence[1] = 0.81;
    const freed = geometry(); freed.freeAirPositions[0] = 1.5;
    for (const changed of [moved, turned, confident, freed, geometry({ voxelSizeMeters: 0.05 }), geometry({ occupancyFineVoxelSizeMeters: 0.07 }), geometry({ tsdfPointCount: 4 })]) {
      expect(tsdfGeometryFingerprint(changed)).not.toBe(base);
    }
  });

  it('is sensitive to which array a value sits in and to the order of the points', () => {
    const swapped = geometry({ tsdfPositions: Float32Array.from([0.1, 0, 0, 0, 0, 0, 0, 0.1, 0.2]) });
    expect(tsdfGeometryFingerprint(swapped)).not.toBe(tsdfGeometryFingerprint(geometry()));
  });
});

describe('post-scan TSDF mesh cache (issue #43 and TASK 06: no second reconstruction of the same scan)', () => {
  it('CACHE-01 holds the mesh for the same geometry and settings, and nothing for anything else', () => {
    const cache = new TsdfMeshCache<string>();
    expect(cache.get('fp-A', 'baseline')).toBeNull();
    cache.set('fp-A', 'baseline', 'meshA');
    expect(cache.get('fp-A', 'baseline')).toBe('meshA');
    expect(cache.get('fp-B', 'baseline')).toBeNull();
    expect(cache.get('fp-A', 'optional-ai')).toBeNull();            /* CACHE-05: other settings can change the geometry: a miss, never a stale hit */
  });

  it('CACHE-02 a second viewer asking for the same scan gets the first viewer\'s mesh (one shared cache)', () => {
    const cache = new TsdfMeshCache<string>();
    const fingerprint = tsdfGeometryFingerprint(geometry());
    cache.set(fingerprint, 'baseline', 'computed by the result viewer');
    const decodedAgain = tsdfGeometryFingerprint(geometry({ tsdfPositions: geometry().tsdfPositions.slice() }));
    expect(cache.get(decodedAgain, 'baseline')).toBe('computed by the result viewer');
  });

  it('keeps a few entries, least recently used first: a merged scan does not evict the one just looked at', () => {
    const cache = new TsdfMeshCache<string>(() => 0, { maxEntries: 2, maxBytes: 1e9 });
    cache.set('A', 's', 'meshA');
    cache.set('B', 's', 'meshB');
    expect(cache.get('A', 's')).toBe('meshA');                      /* A is now the most recently used */
    cache.set('C', 's', 'meshC');                                   /* evicts B, the least recently used */
    expect(cache.get('B', 's')).toBeNull();
    expect(cache.get('A', 's')).toBe('meshA');
    expect(cache.get('C', 's')).toBe('meshC');
    expect(cache.size).toBe(2);
  });

  it('CACHE-06 never holds more than its byte budget: the oldest entries go first, and one mesh larger than the budget is not kept', () => {
    const cache = new TsdfMeshCache<{ bytes: number }>((mesh) => mesh.bytes, { maxEntries: 10, maxBytes: 100 });
    cache.set('A', 's', { bytes: 40 });
    cache.set('B', 's', { bytes: 40 });
    cache.set('C', 's', { bytes: 40 });                              /* 120 > 100: A goes */
    expect(cache.get('A', 's')).toBeNull();
    expect(cache.totalBytes).toBe(80);
    cache.set('HUGE', 's', { bytes: 500 });
    expect(cache.get('HUGE', 's')).toBeNull();
    expect(cache.totalBytes).toBeLessThanOrEqual(100);
  });

  it('re-setting the same key replaces it instead of growing, and clear() gives everything back (memory pressure)', () => {
    const cache = new TsdfMeshCache<{ bytes: number }>((mesh) => mesh.bytes);
    cache.set('A', 's', { bytes: 10 });
    cache.set('A', 's', { bytes: 20 });
    expect(cache.size).toBe(1);
    expect(cache.totalBytes).toBe(20);
    cache.clear();
    expect(cache.size).toBe(0);
    expect(cache.get('A', 's')).toBeNull();
  });

  it('keys the settings by what changes the geometry only', () => {
    expect(tsdfSettingsKey({ allowOptionalAi: false })).toBe('baseline');
    expect(tsdfSettingsKey({ allowOptionalAi: true })).toBe('optional-ai');
  });
});

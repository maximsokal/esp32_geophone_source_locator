import { describe, expect, it } from 'vitest';
import { TsdfMeshCache, tsdfSettingsKey } from '../../src/xr/tsdfMeshCache';

describe('post-scan TSDF mesh cache (issue #43: no second reconstruction of the same scan)', () => {
  const scanA = { id: 'A' };
  const scanB = { id: 'B' };

  it('returns the held mesh for the same snapshot object and settings, and nothing for anything else', () => {
    const cache = new TsdfMeshCache<object, string>();
    expect(cache.get(scanA, 'baseline')).toBeNull();
    cache.set(scanA, 'baseline', 'meshA');
    expect(cache.get(scanA, 'baseline')).toBe('meshA');
    expect(cache.get(scanB, 'baseline')).toBeNull();               /* another scan or a continuation is a different snapshot object */
    expect(cache.get({ ...scanA }, 'baseline')).toBeNull();        /* equal content in a new object is not the same scan identity */
    expect(cache.get(scanA, 'optional-ai')).toBeNull();            /* other settings can change the geometry: a miss, never a stale hit */
  });

  it('keeps one entry: a newer scan replaces the older one, and clear() drops it', () => {
    const cache = new TsdfMeshCache<object, string>();
    cache.set(scanA, 'baseline', 'meshA');
    cache.set(scanB, 'baseline', 'meshB');
    expect(cache.get(scanA, 'baseline')).toBeNull();
    expect(cache.get(scanB, 'baseline')).toBe('meshB');
    cache.clear();
    expect(cache.get(scanB, 'baseline')).toBeNull();
  });

  it('keys the settings by what changes the geometry only', () => {
    expect(tsdfSettingsKey({ allowOptionalAi: false })).toBe('baseline');
    expect(tsdfSettingsKey({ allowOptionalAi: true })).toBe('optional-ai');
  });
});

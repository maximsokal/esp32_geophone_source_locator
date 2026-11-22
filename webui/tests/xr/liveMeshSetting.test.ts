import { describe, expect, it } from 'vitest';
import { readLiveMeshMode, writeLiveMeshMode } from '../../src/xr/liveMeshSetting';

const storageOf = (initial: Record<string, string> = {}) => {
  const data = { ...initial };
  return { data, getItem: (key: string) => data[key] ?? null, setItem: (key: string, value: string) => { data[key] = value; } };
};

describe('the live mesh setting', () => {
  it('is Auto when nothing, or something unknown, is stored', () => {
    expect(readLiveMeshMode(storageOf())).toBe('auto');
    expect(readLiveMeshMode(storageOf({ 'geolocator.liveMesh': 'sometimes' }))).toBe('auto');
    expect(readLiveMeshMode(null)).toBe('auto');
  });
  it('round-trips each mode', () => {
    for (const mode of ['auto', 'on', 'off'] as const) {
      const storage = storageOf();
      writeLiveMeshMode(mode, storage);
      expect(readLiveMeshMode(storage)).toBe(mode);
    }
  });
  it('survives a storage that throws', () => {
    const broken = { getItem: () => { throw new Error('blocked'); }, setItem: () => { throw new Error('blocked'); } };
    expect(readLiveMeshMode(broken)).toBe('auto');
    expect(() => writeLiveMeshMode('on', broken)).not.toThrow();
  });
});

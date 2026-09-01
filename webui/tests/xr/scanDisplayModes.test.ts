import { describe, expect, it } from 'vitest';

import {
  displayFingerprint,
  displayForScanMode,
  scanModeLabel,
  type ScanMode,
} from '../../src/xr/scanDisplayModes';

const modes: ScanMode[] = ['quality', 'surface', 'search-volume', 'mesh'];

describe('scan result display modes', () => {
  it('keeps every operator mode semantically distinct', () => {
    expect(displayForScanMode('quality')).toMatchObject({
      goodPoints: true,
      badPoints: true,
      mesh: true,
      freeAir: false,
      unknownSpace: false,
    });
    expect(displayForScanMode('surface')).toMatchObject({
      goodPoints: true,
      badPoints: false,
      mesh: true,
      freeAir: false,
      unknownSpace: false,
    });
    expect(displayForScanMode('search-volume')).toMatchObject({
      goodPoints: true,
      badPoints: false,
      mesh: false,
      freeAir: true,
      unknownSpace: true,
    });
    expect(displayForScanMode('mesh')).toMatchObject({
      goodPoints: false,
      badPoints: false,
      mesh: true,
      freeAir: false,
      unknownSpace: false,
    });

    const fingerprints = modes.map((mode) => displayFingerprint(displayForScanMode(mode)));
    expect(new Set(fingerprints).size).toBe(modes.length);
  });

  it('returns fresh display objects so one toggle cannot corrupt later presets', () => {
    const first = displayForScanMode('quality');
    first.badPoints = false;
    first.mesh = false;
    const second = displayForScanMode('quality');
    expect(second.badPoints).toBe(true);
    expect(second.mesh).toBe(true);
  });

  it('labels presets and custom layer combinations explicitly', () => {
    expect(scanModeLabel('quality')).toBe('QUALITY + MESH');
    expect(scanModeLabel('surface')).toBe('STABLE SURFACE');
    expect(scanModeLabel('search-volume')).toBe('SEARCH VOLUME');
    expect(scanModeLabel('mesh')).toBe('SURFACE MESH');
    expect(scanModeLabel(null)).toBe('CUSTOM LAYERS');
  });
});

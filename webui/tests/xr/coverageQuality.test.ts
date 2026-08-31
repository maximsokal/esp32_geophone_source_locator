import { describe, expect, it } from 'vitest';

import {
  countCoverageQualityBands,
  coverageQualityBand,
} from '../../src/xr/coverageQuality';

describe('coverage quality bands', () => {
  it('keeps weak improving and stable visually distinct at the boundaries', () => {
    expect(coverageQualityBand(0)).toBe('weak');
    expect(coverageQualityBand(0.4199)).toBe('weak');
    expect(coverageQualityBand(0.42)).toBe('improving');
    expect(coverageQualityBand(0.7199)).toBe('improving');
    expect(coverageQualityBand(0.72)).toBe('stable');
    expect(coverageQualityBand(1)).toBe('stable');
  });

  it('clamps malformed confidence instead of silently turning it green', () => {
    expect(coverageQualityBand(Number.NaN)).toBe('weak');
    expect(coverageQualityBand(-5)).toBe('weak');
    expect(coverageQualityBand(8)).toBe('stable');
  });

  it('counts every rendered confidence exactly once', () => {
    expect(countCoverageQualityBands(new Float32Array([0.1, 0.41, 0.42, 0.6, 0.72, 0.9]))).toEqual({
      weak: 2,
      improving: 2,
      stable: 2,
    });
  });
});

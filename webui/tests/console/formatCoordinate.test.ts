import { describe, expect, it } from 'vitest';
import { formatCoordinate } from '../../src/domain/device';

const MISSING = '\u2014';

describe('formatCoordinate', () => {
  it('formats finite values with six decimals', () => {
    expect(formatCoordinate(50.4501)).toBe('50.450100');
    expect(formatCoordinate(-0.5)).toBe('-0.500000');
  });

  it('does not throw for the null the firmware emits for non-finite floats', () => {
    expect(formatCoordinate(null)).toBe(MISSING);
    expect(formatCoordinate(Number.NaN)).toBe(MISSING);
    expect(formatCoordinate(Number.POSITIVE_INFINITY)).toBe(MISSING);
  });
});

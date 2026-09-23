import { describe, expect, it } from 'vitest';

import { staLtaThresholdIssue } from '../../src/features/objects/LocalizationCalculatePanel';

describe('staLtaThresholdIssue (MAX-210)', () => {
  it('accepts a threshold below the LTA/STA bound', () => {
    expect(staLtaThresholdIssue(5, 50, 3.5)).toBeNull();
  });

  it('rejects a threshold the nested STA/LTA ratio can never reach', () => {
    expect(staLtaThresholdIssue(10, 50, 5)).toMatch(/at most 5\.00/);
    expect(staLtaThresholdIssue(10, 50, 6)).toMatch(/can never reach 6/);
  });

  it('rejects an LTA window that is not longer than STA', () => {
    expect(staLtaThresholdIssue(50, 50, 2)).toMatch(/LTA window must be longer/);
  });

  it('defers until every field parses', () => {
    expect(staLtaThresholdIssue(null, 50, 3)).toBeNull();
  });
});

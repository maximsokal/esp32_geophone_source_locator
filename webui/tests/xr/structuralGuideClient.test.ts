import { describe, expect, it } from 'vitest';

import { isCurrentStructuralGuideWorkerResult } from '../../src/xr/structuralGuideProtocol';

describe('structural guide worker epoch guard', () => {
  it('accepts only the matching request from the active scan epoch', () => {
    expect(isCurrentStructuralGuideWorkerResult(
      { requestId: 7, epoch: 3 },
      { id: 7, epoch: 3 },
      3,
    )).toBe(true);
  });

  it('rejects a result from the previous scan even when request ids look plausible', () => {
    expect(isCurrentStructuralGuideWorkerResult(
      { requestId: 7, epoch: 2 },
      { id: 7, epoch: 2 },
      3,
    )).toBe(false);
  });

  it('rejects an old request after a new analysis has replaced the pending slot', () => {
    expect(isCurrentStructuralGuideWorkerResult(
      { requestId: 7, epoch: 3 },
      { id: 8, epoch: 3 },
      3,
    )).toBe(false);
  });

  it('rejects results when there is no pending analysis', () => {
    expect(isCurrentStructuralGuideWorkerResult({ requestId: 7, epoch: 3 }, null, 3)).toBe(false);
  });
});

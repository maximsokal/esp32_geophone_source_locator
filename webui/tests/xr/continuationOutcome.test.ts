import { describe, expect, it } from 'vitest';

import { ContinuationLedger, continuationGate, type ContinuationOutcome } from '../../src/xr/continuationOutcome';

const done = (state: ContinuationOutcome['state'], code = 'x'): ContinuationOutcome => ({ state, code, messageKey: state === 'success' ? null : 'scan.continuation.cancelled' });

describe('ContinuationLedger: exactly one terminal state per Continue', () => {
  it('accepts the first outcome of the open operation and nothing after it', () => {
    const seen: ContinuationOutcome[] = [];
    const ledger = new ContinuationLedger((o) => seen.push(o));
    const token = ledger.begin();
    expect(ledger.activeToken).toBe(token);
    expect(ledger.settle(token, done('rejected'))).toBe(true);
    expect(ledger.settle(token, done('success'))).toBe(false);
    expect(ledger.settleActive(done('cancelled'))).toBe(false);
    expect(ledger.activeToken).toBeNull();
    expect(seen.map((o) => o.state)).toEqual(['rejected']);
  });

  it('drops a late outcome of a superseded operation and keeps the new one open', () => {
    const seen: ContinuationOutcome[] = [];
    const ledger = new ContinuationLedger((o) => seen.push(o));
    const first = ledger.begin();
    ledger.settleActive(done('cancelled'));
    const second = ledger.begin();
    expect(ledger.settle(first, done('success')), 'a stale callback of the first operation').toBe(false);
    expect(ledger.activeToken).toBe(second);
    expect(ledger.settle(second, done('success'))).toBe(true);
    expect(seen.map((o) => o.state)).toEqual(['cancelled', 'success']);
  });

  it('settleActive does nothing when no operation is open', () => {
    const ledger = new ContinuationLedger(() => { throw new Error('must not be called'); });
    expect(ledger.settleActive(done('cancelled'))).toBe(false);
  });
});

describe('continuationGate: a Continue is merged only through a tag lock', () => {
  const lock = { yaw: 0, tx: 0, ty: 0, tz: 0 };
  it('CONT-01 a lock passes', () => { expect(continuationGate(lock, null)).toEqual({ ok: true }); });
  it('CONT-02/03 fewer than three tags found is "tags_not_found"', () => {
    for (const found of [0, 1, 2]) expect(continuationGate(null, { baseTags: 4, found, needed: 3, reason: 'need_more_tags' })).toEqual({ ok: false, code: 'tags_not_found' });
  });
  it('CONT-04 three tags that the lock found unusable keep the lock\'s own reason', () => {
    for (const reason of ['collinear', 'distance_mismatch', 'residual_too_high'] as const) {
      expect(continuationGate(null, { baseTags: 4, found: 3, needed: 3, reason })).toEqual({ ok: false, code: reason });
    }
  });
  it('a saved scan with too few usable tags is its own reason, whatever was seen', () => {
    expect(continuationGate(null, { baseTags: 2, found: 3, needed: 3, reason: null })).toEqual({ ok: false, code: 'base_tags_insufficient' });
  });
  it('no summary and no lock (an unknown cause) is still a rejection, never a silent merge by overlap', () => {
    expect(continuationGate(null, null)).toEqual({ ok: false, code: 'tags_not_locked' });
    expect(continuationGate(null, { baseTags: 4, found: 3, needed: 3, reason: null })).toEqual({ ok: false, code: 'tags_not_locked' });
  });
});

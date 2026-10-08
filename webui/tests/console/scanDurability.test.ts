import { describe, expect, it } from 'vitest';

import {
  authoritativeSnapshot,
  beginProvisionalScan,
  captureAborted,
  captureStarted,
  continueAvailable,
  confirmScanCommitted,
  confirmScanNotCommitted,
  emptyScanDurability,
  localizationReady,
  markScanUnknown,
  markScanVerifying,
  previousKeptProvable,
  reconcileDurableScan,
  resultControlsAvailable,
  savedScanAvailable,
  shouldShowScanningPresentation,
  setScanDerivativesReady,
  viewportProvenance,
  viewportSnapshot,
} from '../../src/xr/scanDurability';

const scanAId = 'scan-crc32:aaaaaaaa';
const scanBId = 'scan-crc32:bbbbbbbb';

function durableScanA() {
  return reconcileDurableScan(emptyScanDurability<string>(), 'scan A', scanAId, true);
}

function expectScanARestored(state: ReturnType<typeof durableScanA>): void {
  expect(state.phase).toBe('durable');
  expect(state.durableId).toBe(scanAId);
  expect(state.durableSnapshot).toBe('scan A');
  expect(authoritativeSnapshot(state)).toBe('scan A');
  expect(viewportSnapshot(state)).toBe('scan A');
  expect(viewportProvenance(state)).toBe('durable');
  expect(savedScanAvailable(state)).toBe(true);
  expect(continueAvailable(state)).toBe(true);
}

describe('scan durability selectors', () => {
  it('keeps a candidate provisional through verification and unknown outcomes until committed', () => {
    const empty = emptyScanDurability<string>();
    const candidate = beginProvisionalScan(empty, 'scan B', 'scan-crc32:bbbbbbbb');
    expect(viewportSnapshot(candidate)).toBe('scan B');
    expect(viewportProvenance(candidate)).toBe('provisional');
    expect(localizationReady(candidate)).toBe(false);
    expect(previousKeptProvable(candidate)).toBe(false);

    const verifying = markScanVerifying(candidate);
    const unknown = markScanUnknown(verifying);
    expect(viewportProvenance(verifying)).toBe('provisional');
    expect(viewportSnapshot(unknown)).toBe('scan B');
    expect(localizationReady(unknown)).toBe(false);

    const committed = confirmScanCommitted(unknown, 'scan-crc32:bbbbbbbb');
    expect(viewportSnapshot(committed)).toBe('scan B');
    expect(viewportProvenance(committed)).toBe('durable');
    expect(localizationReady(committed)).toBe(false);
    expect(localizationReady(setScanDerivativesReady(committed, true))).toBe(true);

    const replacedAgain = markScanUnknown(setScanDerivativesReady(committed, true));
    expect(viewportProvenance(replacedAgain)).toBe('provisional');
    expect(localizationReady(replacedAgain)).toBe(false);
  });

  it('restores only a provably retained durable snapshot after a rejected candidate', () => {
    const durable = reconcileDurableScan(emptyScanDurability<string>(), 'scan A', 'scan-crc32:aaaaaaaa', true);
    const candidate = beginProvisionalScan(durable, 'scan B', 'scan-crc32:bbbbbbbb');
    const rejected = confirmScanNotCommitted(candidate, true);

    expect(previousKeptProvable(rejected)).toBe(true);
    expect(viewportSnapshot(rejected)).toBe('scan A');
    expect(viewportProvenance(rejected)).toBe('durable');
    expect(localizationReady(rejected)).toBe(true);

    const unproven = confirmScanNotCommitted(candidate, false);
    expect(previousKeptProvable(unproven)).toBe(false);
    expect(viewportSnapshot(unproven)).toBe('scan B');
    expect(viewportProvenance(unproven)).toBe('provisional');
    expect(localizationReady(unproven)).toBe(false);
  });

  it('keeps durable A and its storage identity after Fresh startup rejects requestSession', () => {
    // RED before this addendum: startup unconditionally labeled storage as scanning; the pre-durability App path also cleared A.
    const started = captureStarted(durableScanA(), 'fresh');
    const failed = captureAborted(started);

    expect(shouldShowScanningPresentation(started)).toBe(false);
    expect(shouldShowScanningPresentation(captureStarted(emptyScanDurability<string>(), 'fresh'))).toBe(true);
    expectScanARestored(failed);
    expect(failed.captureActive).toBe(false);
    expect(failed.captureIntent).toBeNull();
    expect(localizationReady(failed)).toBe(true);
  });

  it('keeps durable A after GL or reference-space setup fails after session grant', () => {
    // RED before this addendum: setup failure had no capture-abort transition, so the startup scanning status remained after XR failed.
    const started = captureStarted(durableScanA(), 'fresh');
    const failed = captureAborted(started);

    expect(shouldShowScanningPresentation(started)).toBe(false);
    expectScanARestored(failed);
    expect(failed.durableId).toBe(scanAId);
  });

  it('keeps durable A when the user agent interrupts Fresh before Finish', () => {
    // RED before this addendum: interruption had no App transition to end capture while preserving A's storage identity and presentation.
    const started = captureStarted(durableScanA(), 'fresh');
    const interrupted = captureAborted(started);

    expectScanARestored(interrupted);
    expect(resultControlsAvailable(interrupted)).toBe(true);
  });

  it('restores the held durable state on the available inactive-session signal during navigation or disposal', () => {
    // RED before this addendum: App ignored inactive-session for scan durability; panel unmount has no callback and remains an open signal gap.
    const started = captureStarted(durableScanA(), 'fresh');
    const disposed = captureAborted(started);

    expectScanARestored(disposed);
  });

  it('keeps Continue base A durable while an extension session is starting or aborts', () => {
    // RED before this addendum: the null callback changed Continue's storage label to scanning even though base A remained selected.
    const started = captureStarted(durableScanA(), 'continue');
    const interrupted = captureAborted(started);

    expectScanARestored(interrupted);
    expect(interrupted.captureIntent).toBeNull();
  });

  it('keeps A authoritative when finished Fresh candidate B is definitely not committed', () => {
    // RED before the original durable/provisional flow: startup removed A, leaving no in-memory base to restore after B's upload failed.
    const started = captureStarted(durableScanA(), 'fresh');
    const candidate = beginProvisionalScan(started, 'scan B', scanBId);
    const failed = confirmScanNotCommitted(candidate, true);

    expect(previousKeptProvable(failed)).toBe(true);
    expect(authoritativeSnapshot(failed)).toBe('scan A');
    expect(viewportSnapshot(failed)).toBe('scan A');
    expect(viewportProvenance(failed)).toBe('durable');
    expect(savedScanAvailable(failed)).toBe(true);
    expect(continueAvailable(failed)).toBe(true);
    expect(localizationReady(failed)).toBe(true);
  });

  it('advances A to B once on confirmed commit and ignores duplicate or late commits', () => {
    // RED before this addendum: the null callback cleared A before the guarded commit event could advance authority to B.
    const candidate = beginProvisionalScan(captureStarted(durableScanA(), 'fresh'), 'scan B', scanBId);
    expect(authoritativeSnapshot(candidate)).toBe('scan A');
    const committed = confirmScanCommitted(candidate, scanBId);
    const duplicate = confirmScanCommitted(committed, scanBId);
    const lateWrongId = confirmScanCommitted(committed, scanAId);

    expect(committed.phase).toBe('durable');
    expect(committed.durableSnapshot).toBe('scan B');
    expect(committed.durableId).toBe(scanBId);
    expect(authoritativeSnapshot(committed)).toBe('scan B');
    expect(duplicate).toBe(committed);
    expect(lateWrongId).toBe(committed);
  });
});

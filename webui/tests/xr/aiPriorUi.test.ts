import { describe, expect, it } from 'vitest';

import { describeAiPrior } from '../../src/xr/aiPriorUi';

const base = {
  enabled: true,
  phase: 'running' as const,
  providerStatus: 'disabled' as const,
  hasStagedPostScanFrame: true,
  busy: false,
  calibrationConfidence: 0,
  calibrationModel: null,
  error: null,
};

describe('optional AI UI state', () => {
  it('shows live enabled AI as post-scan pending instead of misleading 0% calibration', () => {
    const state = describeAiPrior(base);
    expect(state.shortStatus).toContain('POST-SCAN');
    expect(state.calibrationLabel).toBe('PENDING AFTER FINISH');
    expect(state.good).toBe(true);
  });

  it('distinguishes staging from actual post-scan inference', () => {
    const running = describeAiPrior({ ...base, phase: 'ended', busy: true, providerStatus: 'loading' });
    expect(running.shortStatus).toBe('RUNNING');
    expect(running.calibrationLabel).toContain('CALIBRATING');
  });

  it('shows accepted calibration model and confidence', () => {
    const ready = describeAiPrior({
      ...base,
      phase: 'ended',
      providerStatus: 'ready',
      busy: false,
      calibrationConfidence: 0.82,
      calibrationModel: 'inverse-affine',
    });
    expect(ready.shortStatus).toBe('READY');
    expect(ready.calibrationLabel).toContain('82%');
    expect(ready.calibrationLabel).toContain('inverse-affine');
    expect(ready.good).toBe(true);
  });

  it('shows calibration failure as fail-open rather than pretending AI was applied', () => {
    const failed = describeAiPrior({
      ...base,
      phase: 'ended',
      providerStatus: 'ready',
      error: 'physical-anchor calibration failed',
    });
    expect(failed.shortStatus).toBe('NOT APPLIED');
    expect(failed.calibrationLabel).toBe('CALIBRATION FAILED');
    expect(failed.good).toBe(false);
  });

  it('shows an ended scan with no staged RGB+depth packet as not applied', () => {
    const unstaged = describeAiPrior({
      ...base,
      phase: 'ended',
      providerStatus: 'disabled',
      hasStagedPostScanFrame: false,
    });
    expect(unstaged.shortStatus).toBe('NOT APPLIED');
    expect(unstaged.calibrationLabel).toBe('NO STAGED RGB+DEPTH');
    expect(unstaged.good).toBe(false);
  });
});

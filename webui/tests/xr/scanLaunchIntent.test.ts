import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  clearScanLaunchIntent, consumeScanLaunchIntent, hasScanLaunchIntent, requestScanLaunch, SCAN_LAUNCH_INTENT_MAX_AGE_MS,
} from '../../src/xr/scanLaunchIntent';

function activation(isActive: boolean | undefined): void {
  vi.stubGlobal('navigator', isActive === undefined ? {} : { userActivation: { isActive } });
}

describe('one-tap scan launch intent (issue #21)', () => {
  beforeEach(() => { clearScanLaunchIntent(); });
  afterEach(() => { clearScanLaunchIntent(); vi.unstubAllGlobals(); });

  it('remembers a tap only while the browser activation is live', () => {
    activation(true);
    expect(requestScanLaunch('OBJ', 'S1', 1000)).toBe(true);
    expect(hasScanLaunchIntent()).toBe(true);
    clearScanLaunchIntent();
    activation(false);
    expect(requestScanLaunch('OBJ', 'S1', 1000)).toBe(false);
    expect(hasScanLaunchIntent()).toBe(false);
  });

  it('never fakes an activation: no userActivation API, or an inactive one, means no intent', () => {
    activation(undefined);
    expect(requestScanLaunch('OBJ', 'S1', 1000)).toBe(false);
    vi.stubGlobal('navigator', { userActivation: {} });
    expect(requestScanLaunch('OBJ', 'S1', 1000)).toBe(false);
    expect(hasScanLaunchIntent()).toBe(false);
  });

  it('is consumed exactly once and only for the object/session that was tapped', () => {
    activation(true);
    requestScanLaunch('OBJ', 'S1', 1000);
    expect(consumeScanLaunchIntent('OBJ', 'S1', 1500)).toBe(true);
    expect(consumeScanLaunchIntent('OBJ', 'S1', 1600)).toBe(false);   /* a remount or a second render cannot repeat it */

    requestScanLaunch('OBJ', 'S1', 2000);
    expect(consumeScanLaunchIntent('OBJ', 'S2', 2100)).toBe(false);   /* another session */
    expect(consumeScanLaunchIntent('OBJ', 'S1', 2100)).toBe(false);   /* and the wrong guess used it up */
    requestScanLaunch('OBJ', 'S1', 3000);
    expect(consumeScanLaunchIntent('OTHER', 'S1', 3100)).toBe(false);
  });

  it('expires inside the browser activation window', () => {
    activation(true);
    requestScanLaunch('OBJ', 'S1', 1000);
    expect(consumeScanLaunchIntent('OBJ', 'S1', 1000 + SCAN_LAUNCH_INTENT_MAX_AGE_MS + 1)).toBe(false);
    requestScanLaunch('OBJ', 'S1', 1000);
    expect(consumeScanLaunchIntent('OBJ', 'S1', 1000 + SCAN_LAUNCH_INTENT_MAX_AGE_MS)).toBe(true);
    expect(SCAN_LAUNCH_INTENT_MAX_AGE_MS).toBeLessThan(5000);
  });

  it('re-checks the activation when it is consumed: a lapsed activation cancels the launch', () => {
    activation(true);
    requestScanLaunch('OBJ', 'S1', 1000);
    activation(false);
    expect(consumeScanLaunchIntent('OBJ', 'S1', 1100)).toBe(false);
    expect(hasScanLaunchIntent()).toBe(false);
  });
});

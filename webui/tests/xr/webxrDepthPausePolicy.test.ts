import { describe, expect, it } from 'vitest';

import {
  depthSensingPauseAction,
  shouldPauseXrScan,
} from '../../src/xr/webxrDepthPausePolicy';

describe('WebXR depth pause policy', () => {
  it('does not request pause control when depth sensing is disabled', () => {
    expect(depthSensingPauseAction(false, true, false)).toBeNull();
    expect(depthSensingPauseAction(false, false, true)).toBeNull();
  });

  it('requests exactly one transition when desired and applied states differ', () => {
    expect(depthSensingPauseAction(true, true, false)).toBe('pause');
    expect(depthSensingPauseAction(true, false, true)).toBe('resume');
    expect(depthSensingPauseAction(true, true, true)).toBeNull();
    expect(depthSensingPauseAction(true, false, false)).toBeNull();
  });

  it('pauses for a hidden document or hidden XR session but not visible-blurred', () => {
    expect(shouldPauseXrScan('hidden', 'visible')).toBe(true);
    expect(shouldPauseXrScan('visible', 'hidden')).toBe(true);
    expect(shouldPauseXrScan('visible', 'visible')).toBe(false);
    expect(shouldPauseXrScan('visible', 'visible-blurred')).toBe(false);
    expect(shouldPauseXrScan('visible', undefined)).toBe(false);
  });
});

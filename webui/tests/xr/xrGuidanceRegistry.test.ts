import { afterEach, describe, expect, it } from 'vitest';

import type { RgbDepthGuidanceFrame } from '../../src/xr/rgbDepthGuidance';
import {
  getFreshRgbGuidance,
  resetXrGuidanceRegistry,
  setLatestRgbGuidance,
} from '../../src/xr/xrGuidanceRegistry';

function guidance(capturedAtMs: number, poseTagged: boolean): RgbDepthGuidanceFrame {
  const width = 16, height = 12;
  return {
    width,
    height,
    luma: new Uint8Array(width * height),
    edgeStrength: new Uint8Array(width * height),
    gradientX: new Int8Array(width * height),
    gradientY: new Int8Array(width * height),
    projectionMatrix: poseTagged ? new Float32Array(16) : undefined,
    worldFromView: poseTagged ? new Float32Array(16) : undefined,
    capturedAtMs,
  };
}

afterEach(() => resetXrGuidanceRegistry());

describe('RGB guidance freshness policy', () => {
  it('keeps legacy/unposed RGB on the caller-requested strict TTL', () => {
    setLatestRgbGuidance(guidance(1000, false));
    expect(getFreshRgbGuidance(1299, 300)).not.toBeNull();
    expect(getFreshRgbGuidance(1301, 300)).toBeNull();
  });

  it('keeps a pose-tagged Raw Camera frame long enough for historical structural reprojection', () => {
    const frame = guidance(1000, true);
    setLatestRgbGuidance(frame);
    expect(getFreshRgbGuidance(1600, 300)).toBe(frame);
  });

  it('still expires pose-tagged RGB instead of turning the visual history into an unbounded stale source', () => {
    setLatestRgbGuidance(guidance(1000, true));
    expect(getFreshRgbGuidance(1650, 300)).not.toBeNull();
    expect(getFreshRgbGuidance(1651, 300)).toBeNull();
  });
});

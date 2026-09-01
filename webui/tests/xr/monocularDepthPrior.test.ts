import { describe, expect, it } from 'vitest';

import {
  calibrateMonocularDepthPrior,
  calibratedPriorDepthMeters,
  type MonocularDepthPriorFrame,
} from '../../src/xr/monocularDepthPrior';

function prior(width = 12, height = 8): MonocularDepthPriorFrame {
  const relativeDepth = new Float32Array(width * height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) relativeDepth[y * width + x] = 0.5 + x * 0.08 + y * 0.03;
  }
  return { width, height, relativeDepth, capturedAtMs: 1, source: 'synthetic' };
}

describe('monocular depth prior calibration', () => {
  it('recovers log-affine metric calibration in the presence of outliers', () => {
    const frame = prior();
    const anchors = [];
    const a = 1.28, b = Math.log(1.7);
    for (let y = 0; y < frame.height; y += 1) {
      for (let x = 0; x < frame.width; x += 2) {
        const u = x / (frame.width - 1), v = y / (frame.height - 1);
        const relative = frame.relativeDepth[y * frame.width + x];
        let metric = Math.exp(a * Math.log(relative) + b);
        if ((x + y) % 11 === 0) metric *= 2.4;
        anchors.push({ u, v, depthMeters: metric });
      }
    }
    const fit = calibrateMonocularDepthPrior(frame, anchors);
    expect(fit).not.toBeNull();
    expect(fit!.scale).toBeCloseTo(a, 1);
    expect(fit!.bias).toBeCloseTo(b, 1);
    expect(fit!.inliers).toBeGreaterThan(20);
  });

  it('calibrates an inverse-depth/disparity-like prior instead of assuming polarity', () => {
    const frame = prior();
    const anchors = [];
    const a = -1.15, b = Math.log(2.4);
    for (let y = 0; y < frame.height; y += 1) {
      for (let x = 0; x < frame.width; x += 2) {
        const u = x / (frame.width - 1), v = y / (frame.height - 1);
        const relative = frame.relativeDepth[y * frame.width + x];
        anchors.push({
          u,
          v,
          depthMeters: Math.exp(a * Math.log(relative) + b),
        });
      }
    }
    const fit = calibrateMonocularDepthPrior(frame, anchors);
    expect(fit).not.toBeNull();
    expect(fit!.scale).toBeLessThan(0);
    expect(fit!.scale).toBeCloseTo(a, 1);
    const nearRelative = calibratedPriorDepthMeters(frame, fit!, 0.05, 0.05)!;
    const farRelative = calibratedPriorDepthMeters(frame, fit!, 0.95, 0.95)!;
    expect(nearRelative).toBeGreaterThan(farRelative);
  });

  it('returns calibrated metric depth only after physical-anchor calibration', () => {
    const frame = prior();
    const a = 0.9, b = Math.log(2.1);
    const anchors = Array.from({ length: 30 }, (_, index) => {
      const x = index % frame.width, y = index % frame.height;
      const relative = frame.relativeDepth[y * frame.width + x];
      return { u: x / (frame.width - 1), v: y / (frame.height - 1), depthMeters: Math.exp(a * Math.log(relative) + b) };
    });
    const fit = calibrateMonocularDepthPrior(frame, anchors)!;
    const u = 0.7, v = 0.4;
    const metric = calibratedPriorDepthMeters(frame, fit, u, v);
    expect(metric).not.toBeNull();
    expect(metric!).toBeGreaterThan(1);
  });

  it('refuses calibration without enough physical anchors', () => {
    expect(calibrateMonocularDepthPrior(prior(), [{ u: 0.5, v: 0.5, depthMeters: 1 }])).toBeNull();
  });
});

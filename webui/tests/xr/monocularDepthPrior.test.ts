import { describe, expect, it } from 'vitest';

import {
  calibrateMonocularDepthPrior,
  calibrateMonocularDepthPriorDetailed,
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

function sampledAnchors(
  frame: MonocularDepthPriorFrame,
  metricForRelative: (relative: number, x: number, y: number) => number,
) {
  const anchors = [];
  for (let y = 0; y < frame.height; y += 1) {
    for (let x = 0; x < frame.width; x += 2) {
      const relative = frame.relativeDepth[y * frame.width + x];
      anchors.push({
        u: x / (frame.width - 1),
        v: y / (frame.height - 1),
        depthMeters: metricForRelative(relative, x, y),
      });
    }
  }
  return anchors;
}

describe('monocular depth prior calibration', () => {
  it('recovers a robust metric mapping in the presence of outliers', () => {
    const frame = prior();
    const a = 1.28, b = Math.log(1.7);
    const anchors = sampledAnchors(frame, (relative, x, y) => {
      const metric = Math.exp(a * Math.log(relative) + b);
      return (x + y) % 11 === 0 ? metric * 2.4 : metric;
    });
    const fit = calibrateMonocularDepthPrior(frame, anchors);
    expect(fit).not.toBeNull();
    expect(fit!.inliers).toBeGreaterThan(20);
    const metric = calibratedPriorDepthMeters(frame, fit!, 0.7, 0.4);
    const relative = frame.relativeDepth[Math.round(0.4 * (frame.height - 1)) * frame.width + Math.round(0.7 * (frame.width - 1))];
    const expected = Math.exp(a * Math.log(relative) + b);
    expect(metric).not.toBeNull();
    expect(metric!).toBeCloseTo(expected, 1);
  });

  it('calibrates an inverse-depth/disparity-like power prior without assuming polarity', () => {
    const frame = prior();
    const a = -1.15, b = Math.log(2.4);
    const anchors = sampledAnchors(frame, (relative) => Math.exp(a * Math.log(relative) + b));
    const fit = calibrateMonocularDepthPrior(frame, anchors);
    expect(fit).not.toBeNull();
    const nearRelative = calibratedPriorDepthMeters(frame, fit!, 0.05, 0.05)!;
    const farRelative = calibratedPriorDepthMeters(frame, fit!, 0.95, 0.95)!;
    expect(nearRelative).toBeGreaterThan(farRelative);
  });

  it('selects affine inverse-depth with shift when relative disparity contains an offset', () => {
    const frame = prior(16, 10);
    const scale = 0.55;
    const bias = 0.18;
    const anchors = sampledAnchors(frame, (relative, x, y) => {
      const metric = 1 / (scale * relative + bias);
      return (x * 3 + y) % 19 === 0 ? metric * 1.9 : metric;
    });
    const detailed = calibrateMonocularDepthPriorDetailed(frame, anchors);
    expect(detailed.calibration).not.toBeNull();
    expect(detailed.calibration!.model).toBe('inverse-affine');
    expect(detailed.calibration!.inliers).toBeGreaterThan(30);
    expect(detailed.diagnostics.selectedModel).toBe('inverse-affine');
    const metric = calibratedPriorDepthMeters(frame, detailed.calibration!, 0.75, 0.55);
    const x = Math.round(0.75 * (frame.width - 1));
    const y = Math.round(0.55 * (frame.height - 1));
    const expected = 1 / (scale * frame.relativeDepth[y * frame.width + x] + bias);
    expect(metric).not.toBeNull();
    expect(metric!).toBeCloseTo(expected, 1);
  });

  it('can select affine metric depth when the relative map is depth-like with shift', () => {
    const frame = prior(16, 10);
    const scale = 1.35;
    const bias = 0.42;
    const anchors = sampledAnchors(frame, (relative) => scale * relative + bias);
    const detailed = calibrateMonocularDepthPriorDetailed(frame, anchors);
    expect(detailed.calibration).not.toBeNull();
    expect(detailed.calibration!.model).toBe('depth-affine');
    const metric = calibratedPriorDepthMeters(frame, detailed.calibration!, 0.25, 0.65);
    const x = Math.round(0.25 * (frame.width - 1));
    const y = Math.round(0.65 * (frame.height - 1));
    expect(metric).toBeCloseTo(scale * frame.relativeDepth[y * frame.width + x] + bias, 2);
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
    const metric = calibratedPriorDepthMeters(frame, fit, 0.7, 0.4);
    expect(metric).not.toBeNull();
    expect(metric!).toBeGreaterThan(1);
  });

  it('reports failed model attempts without inventing calibration from too few anchors', () => {
    const detailed = calibrateMonocularDepthPriorDetailed(prior(), [{ u: 0.5, v: 0.5, depthMeters: 1 }]);
    expect(detailed.calibration).toBeNull();
    expect(detailed.diagnostics.sampleCount).toBe(1);
    expect(detailed.diagnostics.selectedModel).toBeNull();
    expect(detailed.diagnostics.attempts).toHaveLength(3);
    expect(detailed.diagnostics.attempts.every((attempt) => attempt.inliers === 0)).toBe(true);
  });
});

import { describe, expect, it } from 'vitest';

import {
  buildRgbDepthGuidanceFrame,
  buildRgbDepthGuidanceFrameFromLuma,
  guidedDepthInMeters,
  guidanceBoundaryWeight,
  surfaceFusionDepthInMeters,
} from '../../src/xr/rgbDepthGuidance';

function depthSampler(values: number[][]) {
  const height = values.length;
  const width = values[0].length;
  return {
    width,
    height,
    getDepthInMeters(u: number, v: number): number {
      const x = Math.max(0, Math.min(width - 1, Math.round(u * (width - 1))));
      const y = Math.max(0, Math.min(height - 1, Math.round(v * (height - 1))));
      return values[y][x];
    },
  };
}

describe('RGB-guided physical depth', () => {
  it('detects a strong visual edge and preserves its orientation', () => {
    const width = 9;
    const height = 7;
    const luma = new Uint8Array(width * height);
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) luma[y * width + x] = x < 4 ? 20 : 235;
    }
    const frame = buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 10);
    const index = 3 * width + 4;
    expect(frame.edgeStrength[index]).toBeGreaterThan(100);
    expect(Math.abs(frame.gradientX[index])).toBeGreaterThan(Math.abs(frame.gradientY[index]));
  });

  it('preserves top-left-origin camera RGBA for the optional monocular prior', () => {
    // readPixels-style bottom-left-origin source: bottom row first, then top row.
    const rgba = Uint8Array.from([
      255, 0, 0, 255,    0, 255, 0, 255,
      0, 0, 255, 255,    255, 255, 255, 255,
    ]);
    const frame = buildRgbDepthGuidanceFrame(rgba, 2, 2, 2, 2, 'bottom-left', 10);
    expect(frame.rgba).toBeDefined();
    expect(Array.from(frame.rgba!.slice(0, 4))).toEqual([0, 0, 255, 255]);
    expect(Array.from(frame.rgba!.slice(4, 8))).toEqual([255, 255, 255, 255]);
    expect(Array.from(frame.rgba!.slice(8, 12))).toEqual([255, 0, 0, 255]);
    expect(Array.from(frame.rgba!.slice(12, 16))).toEqual([0, 255, 0, 255]);
  });

  it('does not blend foreground and background across an RGB boundary', () => {
    const luma = Uint8Array.from([
      20, 20, 230,
      20, 20, 230,
      20, 20, 230,
    ]);
    const frame = buildRgbDepthGuidanceFrameFromLuma(luma, 3, 3, 10);
    const depth = depthSampler([
      [0.8, 0.8, 2.2],
      [0.8, 0.8, 2.2],
      [0.8, 0.8, 2.2],
    ]);
    const foreground = guidedDepthInMeters(depth, 0.33, 0.5, frame);
    const background = guidedDepthInMeters(depth, 1, 0.5, frame);
    expect(foreground).not.toBeNull();
    expect(background).not.toBeNull();
    expect(foreground!.depthMeters).toBeLessThan(1.0);
    expect(background!.depthMeters).toBeGreaterThan(2.0);
  });

  it('never manufactures metric depth when no physical sample is valid', () => {
    const frame = buildRgbDepthGuidanceFrameFromLuma(new Uint8Array(25).fill(220), 5, 5, 10);
    const depth = depthSampler(Array.from({ length: 5 }, () => Array(5).fill(Number.NaN)));
    expect(guidedDepthInMeters(depth, 0.5, 0.5, frame)).toBeNull();
    expect(Number.isNaN(surfaceFusionDepthInMeters(depth, 0.5, 0.5, frame))).toBe(true);
  });

  it('strongly downweights a physical discontinuity that coincides with an RGB edge', () => {
    const width = 7;
    const height = 5;
    const luma = new Uint8Array(width * height);
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) luma[y * width + x] = x < 3 ? 15 : 245;
    }
    const frame = buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 10);
    const weight = guidanceBoundaryWeight(true, frame, 0.5, 0.5);
    expect(weight).toBeLessThanOrEqual(0.22);
    expect(guidanceBoundaryWeight(false, frame, 0.5, 0.5)).toBe(1);
  });

  it('creates a forbidden fusion band at a strong RGB edge while keeping both side surfaces physical', () => {
    const width = 9;
    const height = 5;
    const luma = new Uint8Array(width * height);
    const depthValues: number[][] = [];
    for (let y = 0; y < height; y += 1) {
      const row: number[] = [];
      for (let x = 0; x < width; x += 1) {
        luma[y * width + x] = x < 4 ? 15 : 245;
        row.push(x < 4 ? 0.85 : 2.25);
      }
      depthValues.push(row);
    }
    const frame = buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 10);
    const depth = depthSampler(depthValues);

    expect(Number.isNaN(surfaceFusionDepthInMeters(depth, 0.5, 0.5, frame))).toBe(true);
    expect(surfaceFusionDepthInMeters(depth, 0.12, 0.5, frame)).toBeLessThan(1.0);
    expect(surfaceFusionDepthInMeters(depth, 0.88, 0.5, frame)).toBeGreaterThan(2.0);
  });
});

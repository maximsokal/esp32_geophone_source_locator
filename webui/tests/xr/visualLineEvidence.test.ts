import { describe, expect, it } from 'vitest';

import { buildRgbDepthGuidanceFrameFromLuma } from '../../src/xr/rgbDepthGuidance';
import { detectVisualLines, VisualLineEvidenceStore } from '../../src/xr/visualLineEvidence';

function image(width: number, height: number, sample: (x: number, y: number) => number): Uint8Array {
  const data = new Uint8Array(width * height);
  for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) data[y * width + x] = sample(x, y);
  return data;
}

describe('visual line evidence', () => {
  it('detects a long contrast boundary', () => {
    const width = 96, height = 64;
    const luma = image(width, height, (x) => x < 46 ? 20 : 235);
    const detected = detectVisualLines(buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 1));
    expect(detected.lines.length).toBeGreaterThan(0);
    const longest = detected.lines[0];
    expect(longest.length).toBeGreaterThan(0.5);
    expect(Math.abs(longest.direction[1])).toBeGreaterThan(0.85);
  });

  it('finds an image junction without needing depth at the exact corner pixel', () => {
    const width = 96, height = 96;
    const luma = image(width, height, (x, y) => x >= 48 && y >= 48 ? 235 : 25);
    const detected = detectVisualLines(buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 1));
    expect(detected.junctions.length).toBeGreaterThan(0);
    const nearest = detected.junctions.reduce((best, item) => {
      const d = Math.hypot(item.position[0] - 0.5, item.position[1] - 0.5);
      return d < best.d ? { d, item } : best;
    }, { d: Infinity, item: detected.junctions[0] });
    expect(nearest.d).toBeLessThan(0.08);
  });

  it('does not turn weak random texture into many structural lines', () => {
    const width = 96, height = 64;
    const luma = image(width, height, (x, y) => 120 + ((x * 17 + y * 31) % 9));
    const detected = detectVisualLines(buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 1));
    expect(detected.lines.length).toBeLessThanOrEqual(2);
  });

  it('keeps a bounded temporal keyframe history', () => {
    const store = new VisualLineEvidenceStore(3);
    for (let keyframeId = 1; keyframeId <= 5; keyframeId += 1) {
      store.add({
        keyframeId,
        capturedAtMs: keyframeId,
        lines: [],
        junctions: [],
        projectionMatrix: new Float32Array(16),
        worldFromView: new Float32Array(16),
      });
    }
    expect(store.size).toBe(3);
    expect(store.snapshot().map((frame) => frame.keyframeId)).toEqual([3, 4, 5]);
  });
});

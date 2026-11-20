import { describe, expect, it } from 'vitest';
import { detectAprilTags } from '../../src/xr/fiducial/tagDetector';
import { buildOverlayQuads } from '../../src/tag-lab/TagLabOverlay';
import { containFit, frameCornerToDisplay } from '../../src/tag-lab/overlayMapping';
import type { LabTagState } from '../../src/tag-lab/labPipeline';
import { rasterTag } from './support/raster';

function stateFor(raster: ReturnType<typeof rasterTag>, tagId: number): LabTagState {
  const detection = detectAprilTags(raster.width, raster.height, raster.rgba).find((d) => d.tagId === tagId)!;
  return { tagId, node: 'G2', status: 'DETECTED 2D', lastSeenMs: 0, corners: detection.corners, frameWidth: raster.width, frameHeight: raster.height, hammingDistance: 0, detectionCount: 1 };
}

describe('tag lab overlay mapping', () => {
  it('letterboxes a 4:3 frame inside a wide box (contain)', () => {
    const fit = containFit({ width: 400, height: 300 }, { width: 800, height: 450 });
    expect(fit.scale).toBe(1.5);
    expect(fit.offsetX).toBe(100);
    expect(fit.offsetY).toBe(0);
  });

  it('puts the real detected tag corners on the drawn tag, in bottom-left-first order, after scaling and letterboxing', () => {
    const raster = rasterTag(2, { left: 100, top: 60 });
    const [quad] = buildOverlayQuads([stateFor(raster, 2)], { width: 800, height: 450 }, false);
    const logical = [[0, 1], [1, 1], [1, 0], [0, 0]] as const; // BL, BR, TR, TL
    logical.forEach(([u, v], index) => {
      const ex = 100 + (raster.left + u * raster.tagSize) * 1.5;
      const ey = (raster.top + v * raster.tagSize) * 1.5;
      expect(Math.hypot(quad.points[index][0] - ex, quad.points[index][1] - ey)).toBeLessThanOrEqual(5);
    });
    expect(quad.label).toBe('G2 (id 2)');
  });

  it('keeps the mapping correct when the box is resized', () => {
    const raster = rasterTag(2);
    const small = buildOverlayQuads([stateFor(raster, 2)], { width: 400, height: 300 }, false)[0];
    const big = buildOverlayQuads([stateFor(raster, 2)], { width: 1200, height: 900 }, false)[0];
    small.points.forEach(([x, y], i) => {
      expect(big.points[i][0]).toBeCloseTo(x * 3, 5);
      expect(big.points[i][1]).toBeCloseTo(y * 3, 5);
    });
  });

  it('mirrors horizontally for a mirrored preview and not otherwise', () => {
    const box = { width: 400, height: 300 };
    const frame = { width: 400, height: 300 };
    const [x] = frameCornerToDisplay([0.25, 0.5], frame, box, false);
    const [mx] = frameCornerToDisplay([0.25, 0.5], frame, box, true);
    expect(x + mx).toBeCloseTo(400, 5);
  });

  it('draws nothing for tags that were never seen', () => {
    const unseen: LabTagState = { tagId: 0, node: 'MAIN', status: 'NOT SEEN', lastSeenMs: null, corners: null, frameWidth: 0, frameHeight: 0, hammingDistance: null, detectionCount: 0 };
    expect(buildOverlayQuads([unseen], { width: 100, height: 100 }, false)).toEqual([]);
  });

  it('maps a portrait frame inside a landscape box with pillarboxing', () => {
    const fit = containFit({ width: 300, height: 400 }, { width: 800, height: 400 });
    expect(fit.scale).toBe(1);
    expect(fit.offsetX).toBe(250);
    expect(fit.offsetY).toBe(0);
  });
});

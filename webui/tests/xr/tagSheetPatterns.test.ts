import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { detectAprilTags } from '../../src/xr/fiducial/tagDetector';

type Pattern = { tag_id: number; data_bits: string; modules: number[][] };
const generated = JSON.parse(readFileSync(new URL('../../../tools/apriltag_sheet/tag_patterns.gen.json', import.meta.url), 'utf8')) as { family: string; tags: Pattern[] };

function transformModules(modules: number[][], rotate: boolean, mirror: boolean): number[][] {
  let output = modules.map((row) => [...row]);
  if (rotate) output = Array.from({ length: 8 }, (_, y) => Array.from({ length: 8 }, (_, x) => output[7 - x][y]));
  if (mirror) output = output.map((row) => [...row].reverse());
  return output;
}

function rasterize(pattern: Pattern, options: { rotate?: boolean; mirror?: boolean; left?: number; top?: number } = {}) {
  const cell = 20;
  const tagSize = cell * 8;
  const left = options.left ?? 20;
  const top = options.top ?? 20;
  const width = tagSize + 2 * cell + 17;
  const height = tagSize + 2 * cell + 23;
  const rgba = new Uint8Array(width * height * 4).fill(255);
  const modules = transformModules(pattern.modules, options.rotate === true, options.mirror === true);
  for (let row = 0; row < 8; row += 1) for (let column = 0; column < 8; column += 1) {
    if (!modules[row][column]) continue;
    for (let y = top + row * cell; y < top + (row + 1) * cell; y += 1) for (let x = left + column * cell; x < left + (column + 1) * cell; x += 1) {
      const offset = (y * width + x) * 4;
      rgba[offset] = 0;
      rgba[offset + 1] = 0;
      rgba[offset + 2] = 0;
    }
  }
  return { rgba, width, height, left, top, tagSize };
}

function assertPoseCornerOrder(detection: ReturnType<typeof detectAprilTags>[number], raster: ReturnType<typeof rasterize>, rotated: boolean): void {
  expect(detection.corners).toHaveLength(4);
  const pixels = detection.corners.map(([x, y]) => [x * (raster.width - 1), y * (raster.height - 1)] as const);
  const twiceArea = pixels.reduce((sum, point, index) => {
    const next = pixels[(index + 1) % pixels.length];
    return sum + point[0] * next[1] - next[0] * point[1];
  }, 0);
  expect(twiceArea).toBeLessThan(0); // tagPose order is bottom-left, bottom-right, top-right, top-left.
  const logicalCorners = [[0, 1], [1, 1], [1, 0], [0, 0]] as const;
  logicalCorners.forEach(([u, v], index) => {
    const x = rotated ? raster.left + (1 - v) * raster.tagSize : raster.left + u * raster.tagSize;
    const y = rotated ? raster.top + u * raster.tagSize : raster.top + v * raster.tagSize;
    expect(Math.hypot(pixels[index][0] - x, pixels[index][1] - y)).toBeLessThanOrEqual(3);
  });
}

describe('generated printable tag36h11 patterns', () => {
  it('rasterizes every generated pattern into only its detector id at normal and rotated offsets', () => {
    expect(generated.family).toBe('tag36h11');
    expect(generated.tags).toHaveLength(5);
    for (const pattern of generated.tags) {
      expect(pattern.data_bits).toMatch(/^[01]{36}$/);
      for (const variant of [
        { rotate: false, left: 20, top: 20 },
        { rotate: true, left: 27, top: 13 },
      ]) {
        const raster = rasterize(pattern, variant);
        const detections = detectAprilTags(raster.width, raster.height, raster.rgba);
        expect(detections.map((entry) => entry.tagId)).toEqual([pattern.tag_id]);
        assertPoseCornerOrder(detections[0], raster, variant.rotate);
      }
    }
  });

  it('does not accept a horizontally mirrored print as the same tag id', () => {
    for (const pattern of generated.tags) {
      const raster = rasterize(pattern, { mirror: true, left: 24, top: 18 });
      expect(detectAprilTags(raster.width, raster.height, raster.rgba).some((entry) => entry.tagId === pattern.tag_id)).toBe(false);
    }
  });
});

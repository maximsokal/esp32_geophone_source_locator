import { describe, expect, it } from 'vitest';

import { AR } from '../../src/xr/fiducial/jsAruco2';
import { refineTagCorners, type PixelQuad } from '../../src/xr/fiducial/cornerRefine';
import { detectAprilTags, mapArucoCornersToTagPose } from '../../src/xr/fiducial/tagDetector';
import { estimateTagPose } from '../../src/xr/fiducial/tagPose';
import {
  projectToImage, projectionMatrix, worldFromView, worldToProject,
  type CameraIntrinsics, type CameraPose, type Vec3,
} from './support/syntheticDepthScene';

const WIDTH = 960;
const HEIGHT = 540;
const intrinsics: CameraIntrinsics = { width: WIDTH, height: HEIGHT, fovYDeg: 45 };
const camera: CameraPose = { eye: [0, 0, 0], target: [0, 1, 0] };
const projection = projectionMatrix(intrinsics);
const cameraToScan = worldFromView(camera);
const TAG_SIZE_M = 0.032;

function transformX(point: Vec3, degrees: number, distance: number): Vec3 {
  const angle = degrees * Math.PI / 180; const c = Math.cos(angle); const s = Math.sin(angle);
  return [point[0], point[1] * c - point[2] * s, point[1] * s + point[2] * c - distance];
}

function pixelPoint(point: Vec3): [number, number] {
  const uv = projectToImage(camera, intrinsics, worldToProject(point));
  if (uv === null) throw new Error('Synthetic tag corner is behind the camera.');
  return [uv[0] * (WIDTH - 1), uv[1] * (HEIGHT - 1)];
}

function solveCellCoordinates(point: readonly [number, number], quad: readonly [readonly [number, number], readonly [number, number], readonly [number, number], readonly [number, number]]): [number, number] | null {
  const [p00, p10, p11, p01] = quad;
  const bx = p00[0] - p10[0] + p11[0] - p01[0]; const by = p00[1] - p10[1] + p11[1] - p01[1];
  let u = 0.5; let v = 0.5;
  for (let i = 0; i < 6; i += 1) {
    const x = p00[0] + u * (p10[0] - p00[0]) + v * (p01[0] - p00[0]) + u * v * bx;
    const y = p00[1] + u * (p10[1] - p00[1]) + v * (p01[1] - p00[1]) + u * v * by;
    const j00 = p10[0] - p00[0] + v * bx; const j01 = p01[0] - p00[0] + u * bx;
    const j10 = p10[1] - p00[1] + v * by; const j11 = p01[1] - p00[1] + u * by;
    const det = j00 * j11 - j01 * j10;
    if (Math.abs(det) < 1e-8) return null;
    const dx = x - point[0]; const dy = y - point[1];
    u -= (j11 * dx - j01 * dy) / det;
    v -= (-j10 * dx + j00 * dy) / det;
  }
  return [u, v];
}

function renderTag(id: number, distance: number, tiltDeg: number, codeOverride?: string): { rgba: Uint8Array; truth: PixelQuad } {
  const rgba = new Uint8Array(WIDTH * HEIGHT * 4);
  rgba.fill(255);
  for (let i = 3; i < rgba.length; i += 4) rgba[i] = 255;
  const code = codeOverride ?? new AR.Dictionary('APRILTAG_36h11')._hex2bin(AR.DICTIONARIES.APRILTAG_36h11.codeList[id], 36);
  const h = TAG_SIZE_M / 2;
  const truth = [
    pixelPoint(transformX([-h, -h, 0], tiltDeg, distance)),
    pixelPoint(transformX([h, -h, 0], tiltDeg, distance)),
    pixelPoint(transformX([h, h, 0], tiltDeg, distance)),
    pixelPoint(transformX([-h, h, 0], tiltDeg, distance)),
  ] as unknown as PixelQuad;
  const displayQuad = [truth[3], truth[2], truth[1], truth[0]] as const;
  const minX = Math.max(0, Math.floor(Math.min(...displayQuad.map((p) => p[0])) - 2));
  const maxX = Math.min(WIDTH - 1, Math.ceil(Math.max(...displayQuad.map((p) => p[0])) + 2));
  const minY = Math.max(0, Math.floor(Math.min(...displayQuad.map((p) => p[1])) - 2));
  const maxY = Math.min(HEIGHT - 1, Math.ceil(Math.max(...displayQuad.map((p) => p[1])) + 2));
  for (let y = minY; y <= maxY; y += 1) for (let x = minX; x <= maxX; x += 1) {
    let black = 0;
    for (const sy of [0.25, 0.75]) for (const sx of [0.25, 0.75]) {
      const uv = solveCellCoordinates([x + sx, y + sy], displayQuad);
      if (uv === null || uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1) continue;
      const column = Math.min(7, Math.floor(uv[0] * 8));
      const row = Math.min(7, Math.floor(uv[1] * 8));
      const isCode = column > 0 && column < 7 && row > 0 && row < 7;
      const white = isCode && code[(row - 1) * 6 + column - 1] === '1';
      if (!white) black += 1;
    }
    const gray = 255 - black * 255 / 4;
    const offset = (y * WIDTH + x) * 4;
    rgba[offset] = gray; rgba[offset + 1] = gray; rgba[offset + 2] = gray; rgba[offset + 3] = 255;
  }
  return { rgba, truth };
}

function cornersError(a: PixelQuad, b: PixelQuad): number {
  return Math.sqrt(a.reduce((sum, point, index) => sum + (point[0] - b[index][0]) ** 2 + (point[1] - b[index][1]) ** 2, 0) / 4);
}

function markerPattern(code: string, cell: number, left: number, top: number): { rgba: Uint8Array; width: number; height: number } {
  const width = 200; const height = 200; const rgba = new Uint8Array(width * height * 4); rgba.fill(255);
  for (let i = 3; i < rgba.length; i += 4) rgba[i] = 255;
  for (let y = 0; y < 8; y += 1) for (let x = 0; x < 8; x += 1) {
    const white = x > 0 && x < 7 && y > 0 && y < 7 && code[(y - 1) * 6 + x - 1] === '1';
    if (!white) for (let py = top + y * cell; py < top + (y + 1) * cell; py += 1) for (let px = left + x * cell; px < left + (x + 1) * cell; px += 1) {
      const at = (py * width + px) * 4; rgba[at] = 0; rgba[at + 1] = 0; rgba[at + 2] = 0;
    }
  }
  return { rgba, width, height };
}

describe('AprilTag 36h11 image detection', () => {
  it.each([0, 1, 2, 3, 4])('detects ID %i, maps its corners, and recovers 32 mm pose at 0.5/1 m and 0/45 degrees', (id) => {
    for (const distance of [0.5, 1]) for (const tilt of [0, 45]) {
      const { rgba, truth } = renderTag(id, distance, tilt);
      const detections = detectAprilTags(WIDTH, HEIGHT, rgba);
      expect(detections.map((detection) => detection.tagId), `id=${id}, distance=${distance}, tilt=${tilt}`).toEqual([id]);
      const detection = detections[0];
      const truthNorm = truth.map(([x, y]) => [x / (WIDTH - 1), y / (HEIGHT - 1)]) as unknown as typeof detection.corners;
      const pixelError = (corners: readonly (readonly [number, number])[]) => cornersError(
        corners.map(([u, v]) => [u * (WIDTH - 1), v * (HEIGHT - 1)]) as unknown as PixelQuad, truth,
      );
      const markers = new AR.Detector({ dictionaryName: 'APRILTAG_36h11', maxHammingDistance: 2 }).detectImage(WIDTH, HEIGHT, rgba);
      expect(markers).toHaveLength(1);
      const rawOrdered = mapArucoCornersToTagPose(markers[0].corners.map((point) => [point.x, point.y] as const) as unknown as PixelQuad);
      const rawNorm = rawOrdered.map(([x, y]) => [x / (WIDTH - 1), y / (HEIGHT - 1)]) as unknown as typeof detection.corners;
      expect(pixelError(detection.corners)).toBeLessThan(pixelError(rawNorm));
      expect(detection.corners.map((corner, index) => Math.hypot(
        corner[0] * (WIDTH - 1) - truth[index][0], corner[1] * (HEIGHT - 1) - truth[index][1],
      )).every((error) => error <= 2)).toBe(true);
      expect(detection.corners).toHaveLength(4);
      expect(truthNorm).toHaveLength(4);

      const pose = estimateTagPose({ corners: detection.corners, projectionMatrix: projection, worldFromView: cameraToScan, tagSizeM: TAG_SIZE_M });
      expect(pose.ok, `id=${id}, distance=${distance}, tilt=${tilt}, reason=${pose.ok ? '' : pose.reason}`).toBe(true);
      if (pose.ok) expect(Math.hypot(...pose.positionScan.map((value, axis) => value - [0, 0, -distance][axis]))).toBeLessThanOrEqual(distance === 0.5 ? 0.005 : 0.01);
    }
  });

  it('rejects random non-dictionary patterns, filtered ID 7, blank frames, and tags below 12 px', () => {
    const dictionary = new AR.Dictionary('APRILTAG_36h11');
    let seed = 1; let randomCode = ''; let rotations: string[] = [];
    do {
      randomCode = '';
      for (let i = 0; i < 36; i += 1) { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; randomCode += seed >>> 31 ? '1' : '0'; }
      rotations = [randomCode];
      for (let turn = 1; turn < 4; turn += 1) {
        const previous = rotations[turn - 1];
        rotations.push(Array.from({ length: 6 }, (_, y) => Array.from({ length: 6 }, (_, x) => previous[(5 - x) * 6 + y]).join('')).join(''));
      }
    } while (rotations.some((candidate) => dictionary.codeList.some((code) => {
      let distance = 0; for (let i = 0; i < 36; i += 1) if (candidate[i] !== code[i]) distance += 1;
      return distance < 2;
    })));
    const nonCode = markerPattern(randomCode, 12, 52, 52);
    expect(detectAprilTags(nonCode.width, nonCode.height, nonCode.rgba)).toEqual([]);
    const id7 = markerPattern(dictionary.codeList[7], 12, 52, 52);
    expect(detectAprilTags(id7.width, id7.height, id7.rgba)).toEqual([]);
    expect(detectAprilTags(WIDTH, HEIGHT, new Uint8Array(WIDTH * HEIGHT * 4).fill(255))).toEqual([]);
    const tiny = markerPattern(dictionary.codeList[0], 1, 95, 95);
    expect(detectAprilTags(tiny.width, tiny.height, tiny.rgba)).toEqual([]);
  });
});

describe('subpixel tag corner refinement', () => {
  it('reduces corner error for a noisy synthetic quadrilateral with subpixel edges', () => {
    const width = 120; const height = 100;
    const truth: PixelQuad = [[18.35, 17.6], [89.4, 21.15], [85.75, 78.35], [14.8, 73.7]];
    const rgba = new Uint8Array(width * height * 4); rgba.fill(255);
    for (let i = 3; i < rgba.length; i += 4) rgba[i] = 255;
    for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
      let inside = 0;
      for (const dy of [0.25, 0.75]) for (const dx of [0.25, 0.75]) {
        let sign = 0; let hit = true;
        for (let edge = 0; edge < 4; edge += 1) {
          const a = truth[edge]; const b = truth[(edge + 1) % 4];
          const side = (b[0] - a[0]) * (y + dy - a[1]) - (b[1] - a[1]) * (x + dx - a[0]);
          if (Math.abs(side) < 1e-6) continue;
          if (sign === 0) sign = Math.sign(side); else if (sign !== Math.sign(side)) hit = false;
        }
        if (hit) inside += 1;
      }
      const gray = inside === 0 ? 255 : inside === 4 ? 0 : Math.round(255 * (1 - inside / 4));
      const noise = ((x * 17 + y * 29) % 7) - 3;
      const value = Math.max(0, Math.min(255, gray + noise)); const at = (y * width + x) * 4;
      rgba[at] = value; rgba[at + 1] = value; rgba[at + 2] = value;
    }
    const raw = truth.map(([x, y]) => [Math.round(x), Math.round(y)]) as unknown as PixelQuad;
    const refined = refineTagCorners(raw, rgba, width, height);
    expect(cornersError(refined, truth)).toBeLessThan(cornersError(raw, truth));
    expect(cornersError(refined, truth)).toBeLessThan(0.3);
  });
});
